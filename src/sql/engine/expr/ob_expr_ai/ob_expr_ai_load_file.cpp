/*
 * Copyright (c) 2025 OceanBase.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#define USING_LOG_PREFIX SQL_ENG
#include "ob_expr_ai_load_file.h"
#include "share/schema/ob_schema_getter_guard.h"
#include "share/schema/ob_location_schema_struct.h"
#include "lib/restore/ob_storage_file.h"
#include "sql/engine/ob_physical_plan_ctx.h"
#include "sql/engine/expr/ob_expr_lob_utils.h"

using namespace oceanbase::common;
using namespace oceanbase::sql;

namespace oceanbase
{
namespace sql
{

ObExprAiLoadFile::ObExprAiLoadFile(common::ObIAllocator &alloc)
    : ObFuncExprOperator(alloc,
                         T_FUN_SYS_AI_LOAD_FILE,
                         N_AI_LOAD_FILE,
                         2,
                         NOT_VALID_FOR_GENERATED_COL,
                         NOT_ROW_DIMENSION)
{
}

ObExprAiLoadFile::~ObExprAiLoadFile()
{
}

int ObExprAiLoadFile::calc_result_type2(ObExprResType &type,
                                        ObExprResType &type1,
                                        ObExprResType &type2,
                                        common::ObExprTypeCtx &type_ctx) const
{
  UNUSED(type_ctx);
  int ret = OB_SUCCESS;
  // location_name: varchar
  type1.set_calc_type(ObVarcharType);
  type1.set_calc_collation_type(ObCharset::get_system_collation());
  // file_name: varchar
  type2.set_calc_type(ObVarcharType);
  type2.set_calc_collation_type(ObCharset::get_system_collation());
  // return BLOB (LongText type)
  type.set_type(ObLongTextType);
  type.set_collation_type(CS_TYPE_UTF8MB4_BIN);
  type.set_collation_level(CS_LEVEL_IMPLICIT);
  type.set_accuracy(ObAccuracy::DDL_DEFAULT_ACCURACY[ObLongTextType]);
  return ret;
}

int ObExprAiLoadFile::resolve_location_path(common::ObIAllocator &allocator,
                                            const common::ObString &location_name,
                                            const common::ObString &file_name,
                                            common::ObString &full_path)
{
  int ret = OB_SUCCESS;
  ObSchemaGetterGuard schema_guard;
  const ObLocationSchema *location_schema = NULL;

  if (OB_ISNULL(GCTX.schema_service_)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("schema service is null", K(ret));
  } else if (OB_FAIL(GCTX.schema_service_->get_tenant_schema_guard(schema_guard))) {
    LOG_WARN("failed to get tenant schema guard", K(ret));
  } else if (OB_FAIL(schema_guard.get_location_schema_by_name(location_name, location_schema))) {
    LOG_WARN("failed to get location schema", K(ret), K(location_name));
  } else if (OB_ISNULL(location_schema)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("location schema is null", K(ret), K(location_name));
  } else {
    // get the base URL (e.g., "file:///tmp/scratch/")
    const char *location_url = location_schema->get_location_url();
    if (OB_ISNULL(location_url)) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("location url is null", K(ret));
    } else {
      // construct the full path: base_url + file_name
      // location_url is "file:///some/path/"
      // we need "file:///some/path/file_name" for ObStorageFileReader
      ObString location_url_str(location_url);
      ObStringBuffer path_buf(&allocator);
      if (OB_FAIL(path_buf.append(location_url_str))) {
        LOG_WARN("failed to append location url", K(ret));
      } else if (OB_FAIL(path_buf.append(file_name))) {
        LOG_WARN("failed to append file name", K(ret));
      } else {
        full_path = path_buf.string();
      }
    }
  }
  return ret;
}

int ObExprAiLoadFile::read_file_to_blob(const common::ObString &file_path,
                                        ObEvalCtx &ctx,
                                        ObDatum &res,
                                        const ObExpr &expr)
{
  int ret = OB_SUCCESS;
  // Use ObStorageFileReader which handles file:// URI prefix automatically
  ObStorageFileReader file_reader;
  int64_t file_length = 0;
  char *buf = NULL;
  int64_t read_size = 0;

  if (OB_FAIL(file_reader.open(file_path, NULL))) {
    LOG_WARN("failed to open file", K(ret), K(file_path));
  } else if (OB_FAIL(file_reader.get_file_length(file_length))) {
    LOG_WARN("failed to get file length", K(ret));
  } else if (file_length <= 0) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("file is empty or invalid", K(ret), K(file_length));
  } else if (OB_ISNULL(buf = static_cast<char *>(ctx.exec_ctx_.get_allocator().alloc(file_length)))) {
    ret = OB_ALLOCATE_MEMORY_FAILED;
    LOG_WARN("failed to allocate memory for file content", K(ret), K(file_length));
  } else if (OB_FAIL(file_reader.pread(buf, file_length, 0, read_size))) {
    LOG_WARN("failed to read file", K(ret), K(file_length));
  } else if (read_size != file_length) {
    ret = OB_IO_ERROR;
    LOG_WARN("read size mismatch", K(ret), K(read_size), K(file_length));
  } else {
    // Set the result as a BLOB (LongText) datum
    ObTextStringResult text_result(ObLongTextType, true, &expr, &ctx, &res);
    ObString file_content(read_size, buf);
    if (OB_FAIL(text_result.init(read_size))) {
      LOG_WARN("failed to init text result", K(ret));
    } else if (OB_FAIL(text_result.append(file_content.ptr(), file_content.length()))) {
      LOG_WARN("failed to append file content", K(ret));
    } else {
      text_result.set_result();
    }
  }

  int close_ret = file_reader.close();
  if (OB_SUCC(ret) && OB_FAIL(close_ret)) {
    LOG_WARN("failed to close file", K(close_ret));
  }
  return ret;
}

int ObExprAiLoadFile::eval_ai_load_file(const ObExpr &expr,
                                        ObEvalCtx &ctx,
                                        ObDatum &res)
{
  INIT_SUCC(ret);
  ObDatum *arg_location = NULL;
  ObDatum *arg_filename = NULL;

  if (OB_FAIL(expr.eval_param_value(ctx, arg_location, arg_filename))) {
    LOG_WARN("evaluate parameters failed", K(ret));
  } else if (arg_location->is_null() || arg_filename->is_null()) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("load_file arguments cannot be null", K(ret));
    LOG_USER_ERROR(OB_INVALID_ARGUMENT, "load_file, arguments cannot be null");
    res.set_null();
  } else {
    ObEvalCtx::TempAllocGuard tmp_alloc_g(ctx);
    MultimodeAlloctor temp_allocator(tmp_alloc_g.get_allocator(), expr.type_, ret);
    lib::ObMallocHookAttrGuard malloc_guard(lib::ObMemAttr(N_AI_LOAD_FILE));

    ObString location_name = arg_location->get_string();
    ObString file_name = arg_filename->get_string();
    ObString full_path;

    if (OB_FAIL(ret)) {
      // skip
    } else if (OB_FAIL(resolve_location_path(temp_allocator, location_name, file_name, full_path))) {
      LOG_WARN("failed to resolve location path", K(ret), K(location_name), K(file_name));
      LOG_USER_ERROR(OB_INVALID_ARGUMENT, "load_file, failed to resolve location");
      res.set_null();
    } else if (OB_FAIL(read_file_to_blob(full_path, ctx, res, expr))) {
      LOG_WARN("failed to read file to blob", K(ret), K(full_path));
      res.set_null();
    }
  }
  return ret;
}

int ObExprAiLoadFile::cg_expr(ObExprCGCtx &expr_cg_ctx,
                              const ObRawExpr &raw_expr,
                              ObExpr &rt_expr) const
{
  UNUSED(raw_expr);
  UNUSED(expr_cg_ctx);
  rt_expr.eval_func_ = ObExprAiLoadFile::eval_ai_load_file;
  return OB_SUCCESS;
}

} // namespace sql
} // namespace oceanbase
