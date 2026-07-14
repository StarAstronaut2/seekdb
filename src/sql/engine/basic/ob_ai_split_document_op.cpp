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

#include "sql/engine/basic/ob_ai_split_document_op.h"
#include "sql/engine/ob_exec_context.h"
#include "sql/engine/expr/ob_expr_lob_utils.h"
#include "lib/allocator/ob_mod_define.h"

namespace oceanbase
{
using namespace common;
namespace sql
{

OB_SERIALIZE_MEMBER((ObAiSplitDocumentSpec, ObOpSpec), content_expr_, params_expr_, column_exprs_);

int ObAiSplitDocumentOp::inner_open()
{
  int ret = OB_SUCCESS;
  first_eval_done_ = false;
  current_chunk_idx_ = 0;
  chunks_.reset();
  return ret;
}

int ObAiSplitDocumentOp::inner_rescan()
{
  int ret = OB_SUCCESS;
  if (OB_FAIL(ObOperator::inner_rescan())) {
    LOG_WARN("failed to inner rescan", K(ret));
  } else {
    first_eval_done_ = false;
    current_chunk_idx_ = 0;
    chunks_.reset();
  }
  return ret;
}

int ObAiSplitDocumentOp::inner_close()
{
  int ret = OB_SUCCESS;
  first_eval_done_ = false;
  current_chunk_idx_ = 0;
  chunks_.reset();
  return ret;
}

void ObAiSplitDocumentOp::destroy()
{
  ObOperator::destroy();
}

int ObAiSplitDocumentOp::inner_get_next_row()
{
  int ret = OB_SUCCESS;
  const ObAiSplitDocumentSpec &spec = MY_SPEC;

  clear_evaluated_flag();
  if (OB_FAIL(ctx_.check_status())) {
    LOG_WARN("failed to check status", K(ret));
  }

  // First call: evaluate content and params, split document, cache results
  if (OB_SUCC(ret) && !first_eval_done_) {
    ObDatum *content_datum = NULL;
    ObDatum *params_datum = NULL;

    if (OB_ISNULL(spec.content_expr_)) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("content_expr_ is null", K(ret));
    } else if (OB_FAIL(spec.content_expr_->eval(eval_ctx_, content_datum))) {
      LOG_WARN("failed to eval content expr", K(ret));
    } else if (content_datum->is_null()) {
      // Empty content, no chunks
      first_eval_done_ = true;
    } else {
      ObEvalCtx::TempAllocGuard tmp_alloc_g(ctx_);
      MultimodeAlloctor temp_allocator(tmp_alloc_g.get_allocator(), 
                                        spec.type_, ret);
      lib::ObMallocHookAttrGuard malloc_guard(lib::ObMemAttr("ai_split_document"));

      ObString content;
      if (OB_FAIL(ObTextStringHelper::read_real_string_data(
          temp_allocator, *content_datum,
          spec.content_expr_->datum_meta_,
          spec.content_expr_->obj_meta_.has_lob_header(),
          content))) {
        LOG_WARN("fail to get real string data", K(ret));
      } else {
        ObString params_json;
        if (OB_NOT_NULL(spec.params_expr_)) {
          if (OB_FAIL(spec.params_expr_->eval(eval_ctx_, params_datum))) {
            LOG_WARN("failed to eval params expr", K(ret));
          } else if (OB_NOT_NULL(params_datum) && !params_datum->is_null()) {
            if (OB_FAIL(ObTextStringHelper::read_real_string_data(
                temp_allocator, *params_datum,
                spec.params_expr_->datum_meta_,
                spec.params_expr_->obj_meta_.has_lob_header(),
                params_json))) {
              LOG_WARN("fail to get real string data for params", K(ret));
            }
          }
        }

        if (OB_SUCC(ret)) {
          ObSEArray<ObSplitChunk, 16> split_result;
          if (OB_FAIL(ObExprAiSplitDocument::split_document(
              temp_allocator, content, params_json, split_result))) {
            LOG_WARN("failed to split document", K(ret));
          } else {
            // Copy chunks to member array (deep copy)
            for (int64_t i = 0; OB_SUCC(ret) && i < split_result.count(); ++i) {
              ObSplitChunk chunk;
              chunk.chunk_id_ = split_result.at(i).chunk_id_;
              chunk.chunk_offset_ = split_result.at(i).chunk_offset_;
              chunk.chunk_length_ = split_result.at(i).chunk_length_;
              
              // Deep copy text
              const ObString &src_text = split_result.at(i).chunk_text_;
              char *buf = NULL;
              if (src_text.length() > 0) {
                if (OB_ISNULL(buf = static_cast<char *>(
                    ctx_.get_allocator().alloc(src_text.length())))) {
                  ret = OB_ALLOCATE_MEMORY_FAILED;
                  LOG_WARN("failed to alloc memory for chunk text", K(ret));
                } else {
                  MEMCPY(buf, src_text.ptr(), src_text.length());
                  chunk.chunk_text_ = ObString(src_text.length(), buf);
                }
              } else {
                chunk.chunk_text_ = ObString();
              }
              
              if (OB_SUCC(ret) && OB_FAIL(chunks_.push_back(chunk))) {
                LOG_WARN("failed to push chunk", K(ret));
              }
            }
            first_eval_done_ = true;
          }
        }
      }
    }
  }

  // Return next row
  if (OB_SUCC(ret)) {
    if (current_chunk_idx_ >= chunks_.count()) {
      ret = OB_ITER_END;
    } else {
      ObSplitChunk &chunk = chunks_.at(current_chunk_idx_);
      
      // Set chunk_id (column 0, INT)
      if (spec.column_exprs_.count() > 0 && OB_NOT_NULL(spec.column_exprs_.at(0))) {
        spec.column_exprs_.at(0)->locate_datum_for_write(eval_ctx_).set_int(chunk.chunk_id_);
        spec.column_exprs_.at(0)->set_evaluated_projected(eval_ctx_);
      }
      
      // Set chunk_offset (column 1, INT)
      if (spec.column_exprs_.count() > 1 && OB_NOT_NULL(spec.column_exprs_.at(1))) {
        spec.column_exprs_.at(1)->locate_datum_for_write(eval_ctx_).set_int(chunk.chunk_offset_);
        spec.column_exprs_.at(1)->set_evaluated_projected(eval_ctx_);
      }
      
      // Set chunk_length (column 2, INT)
      if (spec.column_exprs_.count() > 2 && OB_NOT_NULL(spec.column_exprs_.at(2))) {
        spec.column_exprs_.at(2)->locate_datum_for_write(eval_ctx_).set_int(chunk.chunk_length_);
        spec.column_exprs_.at(2)->set_evaluated_projected(eval_ctx_);
      }
      
      // Set chunk_text (column 3, VARCHAR)
      if (spec.column_exprs_.count() > 3 && OB_NOT_NULL(spec.column_exprs_.at(3))) {
        ObDatum &text_datum = spec.column_exprs_.at(3)->locate_datum_for_write(eval_ctx_);
        text_datum.set_string(chunk.chunk_text_);
        spec.column_exprs_.at(3)->set_evaluated_projected(eval_ctx_);
      }
      
      current_chunk_idx_++;
    }
  }

  return ret;
}

} // end namespace sql
} // end namespace oceanbase
