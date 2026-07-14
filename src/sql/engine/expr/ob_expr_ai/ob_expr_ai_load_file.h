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

#ifndef OCEANBASE_SQL_OB_EXPR_AI_LOAD_FILE_H_
#define OCEANBASE_SQL_OB_EXPR_AI_LOAD_FILE_H_

#include "sql/engine/expr/ob_expr_operator.h"
#include "sql/engine/ob_exec_context.h"

namespace oceanbase
{
namespace sql
{

class ObExprAiLoadFile : public ObFuncExprOperator
{
public:
  explicit ObExprAiLoadFile(common::ObIAllocator &alloc);
  virtual ~ObExprAiLoadFile();

  virtual int calc_result_type2(ObExprResType &type,
                                ObExprResType &type1,
                                ObExprResType &type2,
                                common::ObExprTypeCtx &type_ctx) const override;

  static int eval_ai_load_file(const ObExpr &expr, ObEvalCtx &ctx, ObDatum &res);

  virtual int cg_expr(ObExprCGCtx &expr_cg_ctx,
                      const ObRawExpr &raw_expr,
                      ObExpr &rt_expr) const override;

  virtual bool need_rt_ctx() const override { return true; }

private:
  static int read_file_to_blob(const common::ObString &file_path,
                               ObEvalCtx &ctx,
                               ObDatum &res,
                               const ObExpr &expr);

  static int resolve_location_path(common::ObIAllocator &allocator,
                                   const common::ObString &location_name,
                                   const common::ObString &file_name,
                                   common::ObString &full_path);

  DISALLOW_COPY_AND_ASSIGN(ObExprAiLoadFile);
};

} // namespace sql
} // namespace oceanbase

#endif // OCEANBASE_SQL_OB_EXPR_AI_LOAD_FILE_H_
