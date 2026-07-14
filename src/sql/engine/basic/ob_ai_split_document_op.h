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

#ifndef OCEANBASE_SQL_ENGINE_BASIC_OB_AI_SPLIT_DOCUMENT_OP_H_
#define OCEANBASE_SQL_ENGINE_BASIC_OB_AI_SPLIT_DOCUMENT_OP_H_

#include "sql/engine/ob_operator.h"
#include "sql/engine/basic/ob_chunk_datum_store.h"
#include "sql/engine/expr/ob_expr_ai/ob_expr_ai_split_document.h"

namespace oceanbase
{
namespace sql
{

class ObExpr;
class ObAiSplitDocumentSpec : public ObOpSpec
{
  OB_UNIS_VERSION_V(1);
public:
  ObAiSplitDocumentSpec(common::ObIAllocator &alloc, const ObPhyOperatorType type)
    : ObOpSpec(alloc, type),
      content_expr_(nullptr),
      params_expr_(nullptr),
      column_exprs_(alloc)
  {}
  ObExpr *content_expr_;   // expression for document content
  ObExpr *params_expr_;    // expression for params JSON (nullable)
  common::ObFixedArray<ObExpr*, common::ObIAllocator> column_exprs_;  // 4 output columns
};

class ObAiSplitDocumentOp : public ObOperator
{
public:
  ObAiSplitDocumentOp(ObExecContext &exec_ctx, const ObOpSpec &spec, ObOpInput *input)
    : ObOperator(exec_ctx, spec, input),
      first_eval_done_(false),
      current_chunk_idx_(0),
      chunks_()
  {}

  virtual int inner_open() override;
  virtual int inner_rescan() override;
  virtual int inner_get_next_row() override;
  virtual int inner_close() override;
  virtual void destroy() override;

private:
  bool first_eval_done_;
  int64_t current_chunk_idx_;
  common::ObSEArray<ObSplitChunk, 16> chunks_;

  DISALLOW_COPY_AND_ASSIGN(ObAiSplitDocumentOp);
};

} // end namespace sql
} // end namespace oceanbase

#endif /* OCEANBASE_SQL_ENGINE_BASIC_OB_AI_SPLIT_DOCUMENT_OP_H_ */
