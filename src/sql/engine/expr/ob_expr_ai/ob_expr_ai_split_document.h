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

#ifndef OCEANBASE_SQL_OB_EXPR_AI_SPLIT_DOCUMENT_H_
#define OCEANBASE_SQL_OB_EXPR_AI_SPLIT_DOCUMENT_H_

#include "sql/engine/expr/ob_expr_operator.h"
#include "sql/engine/ob_exec_context.h"
#include "lib/json_type/ob_json_common.h"
#include "lib/string/ob_string.h"

namespace oceanbase
{
namespace sql
{

struct ObSplitChunk
{
  int64_t chunk_id_;
  int64_t chunk_offset_;
  int64_t chunk_length_;
  common::ObString chunk_text_;
};

struct ObSplitParams
{
  ObSplitParams()
    : type_is_markdown_(true),
      by_word_(true),
      max_(256),
      overlap_(0)
  {}
  bool type_is_markdown_;   // true=markdown, false=text
  bool by_word_;            // true=word, false=sentence
  int64_t max_;             // max words/sentences per chunk
  int64_t overlap_;         // overlap between chunks
};

class ObExprAiSplitDocument : public ObFuncExprOperator
{
public:
  explicit ObExprAiSplitDocument(common::ObIAllocator &alloc);
  virtual ~ObExprAiSplitDocument();

  virtual int calc_result_type2(ObExprResType &type,
                                ObExprResType &type1,
                                ObExprResType &type2,
                                common::ObExprTypeCtx &type_ctx) const override;

  static int eval_ai_split_document(const ObExpr &expr, ObEvalCtx &ctx, ObDatum &res);

  virtual int cg_expr(ObExprCGCtx &expr_cg_ctx,
                      const ObRawExpr &raw_expr,
                      ObExpr &rt_expr) const override;

  // Public API for splitting (used by physical operator)
  static int split_document(common::ObIAllocator &allocator,
                            const common::ObString &content,
                            const common::ObString &params_json,
                            common::ObIArray<ObSplitChunk> &chunks);

private:
  static int parse_params(const common::ObString &params_json,
                          ObSplitParams &params);

  // Text splitting
  static int split_by_sentence(common::ObIAllocator &allocator,
                               const common::ObString &content,
                               const ObSplitParams &params,
                               common::ObIArray<ObSplitChunk> &chunks);

  static int split_by_word(common::ObIAllocator &allocator,
                           const common::ObString &content,
                           const ObSplitParams &params,
                           common::ObIArray<ObSplitChunk> &chunks);

  // Markdown splitting
  static int split_markdown(common::ObIAllocator &allocator,
                            const common::ObString &content,
                            const ObSplitParams &params,
                            common::ObIArray<ObSplitChunk> &chunks);

  static int split_markdown_section(common::ObIAllocator &allocator,
                                    const common::ObString &section_heading,
                                    const common::ObString &section_body,
                                    const ObSplitParams &params,
                                    int64_t section_offset,
                                    int64_t &base_chunk_id,
                                    common::ObIArray<ObSplitChunk> &chunks);

  static int split_text_content(common::ObIAllocator &allocator,
                                const common::ObString &content,
                                const ObSplitParams &params,
                                int64_t content_offset,
                                const common::ObString &prefix,
                                int64_t &base_chunk_id,
                                int64_t &next_start_offset,
                                common::ObIArray<ObSplitChunk> &chunks);

  static void find_sentence_boundaries(const common::ObString &text,
                                       common::ObIArray<int64_t> &boundaries);

  DISALLOW_COPY_AND_ASSIGN(ObExprAiSplitDocument);
};

} // namespace sql
} // namespace oceanbase

#endif // OCEANBASE_SQL_OB_EXPR_AI_SPLIT_DOCUMENT_H_
