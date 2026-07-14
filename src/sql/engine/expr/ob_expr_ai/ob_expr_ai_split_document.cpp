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
#include "ob_expr_ai_split_document.h"
#include "lib/oblog/ob_log_module.h"
#include "sql/engine/expr/ob_expr_lob_utils.h"
#include "ob_ai_func_utils.h"

using namespace oceanbase::common;
using namespace oceanbase::sql;

namespace oceanbase
{
namespace sql
{

ObExprAiSplitDocument::ObExprAiSplitDocument(common::ObIAllocator &alloc)
    : ObFuncExprOperator(alloc,
                         T_FUN_SYS_AI_SPLIT_DOCUMENT,
                         N_AI_SPLIT_DOCUMENT,
                         MORE_THAN_ZERO,
                         NOT_VALID_FOR_GENERATED_COL,
                         NOT_ROW_DIMENSION)
{
}

ObExprAiSplitDocument::~ObExprAiSplitDocument()
{
}

int ObExprAiSplitDocument::calc_result_type2(ObExprResType &type,
                                             ObExprResType &type1,
                                             ObExprResType &type2,
                                             common::ObExprTypeCtx &type_ctx) const
{
  UNUSED(type_ctx);
  int ret = OB_SUCCESS;
  // content: varchar
  type1.set_calc_type(ObVarcharType);
  type1.set_calc_collation_type(CS_TYPE_UTF8MB4_BIN);
  // params: varchar (optional, default NULL)
  type2.set_calc_type(ObVarcharType);
  type2.set_calc_collation_type(CS_TYPE_UTF8MB4_BIN);
  // return type: LongText (JSON array of chunks, used by function-table path)
  type.set_type(ObLongTextType);
  type.set_collation_type(CS_TYPE_UTF8MB4_BIN);
  type.set_collation_level(CS_LEVEL_IMPLICIT);
  type.set_accuracy(ObAccuracy::DDL_DEFAULT_ACCURACY[ObLongTextType]);
  return ret;
}

int ObExprAiSplitDocument::parse_params(const common::ObString &params_json,
                                        ObSplitParams &params)
{
  int ret = OB_SUCCESS;
  if (params_json.empty() || params_json.length() == 0) {
    // Use defaults
    return ret;
  }

  // Parse JSON using ObJsonParser
  ObJsonParser json_parser;
  ObJsonNode *root = NULL;
  ObIAllocator *allocator = NULL; // Will be allocated if needed
  // Simple JSON key-value scanning approach using string search
  // Since we need a simple JSON parse for {key:value} pairs, do manual scanning

  // Look for "type" field
  const char *ptr = params_json.ptr();
  int64_t len = params_json.length();

  // Simple state-machine parser for the flat JSON object we expect
  // {"type":"text","by":"word","max":256,"overlap":0}
  // We search for key-value pairs

  // Find "type"
  const char *type_key = NULL;
  if (NULL != (type_key = strstr(ptr, "\"type\""))) {
    const char *val_start = type_key + 6; // skip "type"
    // skip to colon
    while (val_start < ptr + len && *val_start != ':') val_start++;
    val_start++; // skip ':'
    // skip whitespace
    while (val_start < ptr + len && (*val_start == ' ' || *val_start == '\t')) val_start++;
    if (val_start < ptr + len && *val_start == '"') {
      val_start++; // skip opening quote
      if (0 == strncmp(val_start, "text", 4)) {
        params.type_is_markdown_ = false;
      } // else default is markdown
    }
  }

  // Find "by"
  const char *by_key = NULL;
  if (NULL != (by_key = strstr(ptr, "\"by\""))) {
    const char *val_start = by_key + 3; // skip "by"
    while (val_start < ptr + len && *val_start != ':') val_start++;
    val_start++;
    while (val_start < ptr + len && (*val_start == ' ' || *val_start == '\t')) val_start++;
    if (val_start < ptr + len && *val_start == '"') {
      val_start++;
      if (0 == strncmp(val_start, "sentence", 8)) {
        params.by_word_ = false;
      } // else default is word
    }
  }

  // Find "max"
  const char *max_key = NULL;
  if (NULL != (max_key = strstr(ptr, "\"max\""))) {
    const char *val_start = max_key + 4; // skip "max"
    while (val_start < ptr + len && *val_start != ':') val_start++;
    val_start++;
    while (val_start < ptr + len && (*val_start == ' ' || *val_start == '\t')) val_start++;
    if (val_start < ptr + len) {
      char *end = NULL;
      int64_t val = strtoll(val_start, &end, 10);
      if (end > val_start) {
        params.max_ = val;
      }
    }
  }

  // Find "overlap"
  const char *overlap_key = NULL;
  if (NULL != (overlap_key = strstr(ptr, "\"overlap\""))) {
    const char *val_start = overlap_key + 8; // skip "overlap"
    while (val_start < ptr + len && *val_start != ':') val_start++;
    val_start++;
    while (val_start < ptr + len && (*val_start == ' ' || *val_start == '\t')) val_start++;
    if (val_start < ptr + len) {
      char *end = NULL;
      int64_t val = strtoll(val_start, &end, 10);
      if (end > val_start) {
        params.overlap_ = val;
      }
    }
  }

  return ret;
}

void ObExprAiSplitDocument::find_sentence_boundaries(const common::ObString &text,
                                                     common::ObIArray<int64_t> &boundaries)
{
  // Find sentence-ending punctuation: . ! ? as well as Chinese 。！？
  // Each boundary position is the index right AFTER the punctuation + any trailing whitespace
  const char *ptr = text.ptr();
  int64_t len = text.length();
  for (int64_t i = 0; i < len; i++) {
    char c = ptr[i];
    if (c == '.' || c == '!' || c == '?' || 
        (unsigned char)c == 0xE3 && i + 2 < len && // UTF-8 for CJK punctuation
        ((unsigned char)ptr[i+1] == 0x80 && (unsigned char)ptr[i+2] == 0x82) || // 。 (U+3002)
        ((unsigned char)ptr[i+1] == 0x80 && (unsigned char)ptr[i+2] == 0x81) || // ！ (U+FF01)
        ((unsigned char)ptr[i+1] == 0x81 && (unsigned char)ptr[i+2] == 0x9C))   // ？ (U+FF1F)
    {
      // Include the punctuation and skip trailing whitespace/newlines
      int64_t end = i + 1;
      // For multi-byte CJK, advance properly
      if ((unsigned char)c == 0xE3) {
        end = i + 3;
      }
      // Skip whitespace after punctuation
      while (end < len && (ptr[end] == ' ' || ptr[end] == '\t' || ptr[end] == '\n' || ptr[end] == '\r')) {
        end++;
      }
      if (OB_UNLIKELY(boundaries.count() == 0 || boundaries.at(boundaries.count() - 1) < end)) {
        boundaries.push_back(end);
      }
    }
  }
  // Always add the end of text as the last boundary
  if (boundaries.count() == 0 || boundaries.at(boundaries.count() - 1) < len) {
    boundaries.push_back(len);
  }
}

int ObExprAiSplitDocument::split_text_content(common::ObIAllocator &allocator,
                                              const common::ObString &content,
                                              const ObSplitParams &params,
                                              int64_t content_offset,
                                              const common::ObString &prefix,
                                              int64_t &base_chunk_id,
                                              int64_t &next_start_offset,
                                              common::ObIArray<ObSplitChunk> &chunks)
{
  int ret = OB_SUCCESS;
  UNUSED(allocator);

  if (content.empty()) {
    next_start_offset = content_offset + content.length();
    return ret;
  }

  if (params.by_word_) {
    // Split by words (whitespace-separated)
    const char *ptr = content.ptr();
    int64_t len = content.length();

    // Find word boundaries
    ObSEArray<int64_t, 64> word_starts;
    ObSEArray<int64_t, 64> word_ends;
    int64_t i = 0;
    while (i < len) {
      // Skip whitespace
      while (i < len && (ptr[i] == ' ' || ptr[i] == '\t' || ptr[i] == '\n' || ptr[i] == '\r')) {
        i++;
      }
      if (i >= len) break;
      word_starts.push_back(i);
      // Find end of word
      while (i < len && !(ptr[i] == ' ' || ptr[i] == '\t' || ptr[i] == '\n' || ptr[i] == '\r')) {
        i++;
      }
      word_ends.push_back(i);
    }

    if (word_starts.count() == 0) {
      next_start_offset = content_offset + len;
      return ret;
    }

    int64_t num_words = word_starts.count();
    int64_t max_words = params.max_ > 0 ? params.max_ : 256;
    int64_t overlap_words = params.overlap_ >= 0 ? params.overlap_ : 0;
    if (overlap_words >= max_words) {
      overlap_words = max_words - 1;
    }
    int64_t step = max_words - overlap_words;
    if (step <= 0) step = 1;

    for (int64_t start = 0; start < num_words && OB_SUCC(ret); start += step) {
      int64_t end = start + max_words;
      if (end > num_words) end = num_words;

      int64_t chunk_start = word_starts.at(start);
      int64_t chunk_end = word_ends.at(end - 1);
      int64_t chunk_len = chunk_end - chunk_start;

      // Build chunk text: prefix + content segment
      ObString content_segment(chunk_len, content.ptr() + chunk_start);
      ObString chunk_text;
      if (prefix.length() > 0) {
        // prefix already includes newline if needed
        ObStringBuffer buf(&allocator);
        if (OB_FAIL(buf.append(prefix))) {
          LOG_WARN("failed to append prefix", K(ret));
        } else if (OB_FAIL(buf.append(content_segment.ptr(), content_segment.length()))) {
          LOG_WARN("failed to append content segment", K(ret));
        } else {
          chunk_text = buf.string();
        }
      } else {
        chunk_text = content_segment;
      }

      if (OB_SUCC(ret)) {
        ObSplitChunk chunk;
        chunk.chunk_id_ = base_chunk_id++;
        chunk.chunk_offset_ = content_offset + chunk_start;
        chunk.chunk_length_ = chunk_len + (prefix.length() > 0 ? prefix.length() : 0);
        chunk.chunk_text_ = chunk_text;
        if (OB_FAIL(chunks.push_back(chunk))) {
          LOG_WARN("failed to push chunk", K(ret));
        }
      }

      if (end >= num_words) {
        next_start_offset = content_offset + chunk_end;
        break;
      }
    }
    if (OB_SUCC(ret) && chunks.count() > 0) {
      next_start_offset = content_offset + word_ends.at(num_words - 1);
    }
  } else {
    // Split by sentences
    ObSEArray<int64_t, 32> boundaries;
    find_sentence_boundaries(content, boundaries);

    if (boundaries.count() == 0) {
      next_start_offset = content_offset + content.length();
      return ret;
    }

    int64_t num_sentences = boundaries.count();
    int64_t max_sentences = params.max_ > 0 ? params.max_ : 256;
    int64_t overlap_sentences = params.overlap_ >= 0 ? params.overlap_ : 0;
    if (overlap_sentences >= max_sentences) {
      overlap_sentences = max_sentences - 1;
    }
    int64_t step = max_sentences - overlap_sentences;
    if (step <= 0) step = 1;

    int64_t prev_end = 0;
    for (int64_t s = 0; s < num_sentences && OB_SUCC(ret); s += step) {
      int64_t end_idx = s + max_sentences;
      if (end_idx > num_sentences) end_idx = num_sentences;
      int64_t chunk_end = boundaries.at(end_idx - 1);
      int64_t chunk_start = boundaries.at(s) > 0 ? prev_end : 0;

      // If this isn't the first chunk and we're using overlap,
      // the chunk starts at the right position
      if (s > 0) {
        chunk_start = boundaries.at(s - overlap_sentences > 0 ? s - overlap_sentences : 0);
        if (s >= overlap_sentences) {
          chunk_start = boundaries.at(s - overlap_sentences);
        }
      }

      // The actual start for this window
      int64_t actual_start = chunk_start;
      if (s > 0) {
        // For the sliding window, we want to include overlap sentences from previous window
        int64_t overlap_start_idx = s >= overlap_sentences ? s - overlap_sentences : 0;
        actual_start = (overlap_start_idx == 0) ? 0 : (boundaries.at(overlap_start_idx - 1));
      }

      if (actual_start >= chunk_end) {
        continue;
      }

      int64_t actual_len = chunk_end - actual_start;

      ObString content_segment(actual_len, content.ptr() + actual_start);
      ObString chunk_text;
      if (prefix.length() > 0) {
        ObStringBuffer buf(&allocator);
        if (OB_FAIL(buf.append(prefix))) {
          LOG_WARN("failed to append prefix", K(ret));
        } else if (OB_FAIL(buf.append(content_segment.ptr(), content_segment.length()))) {
          LOG_WARN("failed to append content segment", K(ret));
        } else {
          chunk_text = buf.string();
        }
      } else {
        chunk_text = content_segment;
      }

      if (OB_SUCC(ret)) {
        ObSplitChunk chunk;
        chunk.chunk_id_ = base_chunk_id++;
        chunk.chunk_offset_ = content_offset + actual_start;
        chunk.chunk_length_ = actual_len + (prefix.length() > 0 ? prefix.length() : 0);
        chunk.chunk_text_ = chunk_text;
        if (OB_FAIL(chunks.push_back(chunk))) {
          LOG_WARN("failed to push chunk", K(ret));
        }
      }

      prev_end = chunk_end;
      if (end_idx >= num_sentences) {
        next_start_offset = content_offset + chunk_end;
      }
    }
    if (OB_SUCC(ret) && chunks.count() > 0) {
      next_start_offset = content_offset + boundaries.at(boundaries.count() - 1);
    }
  }

  return ret;
}

int ObExprAiSplitDocument::split_by_sentence(common::ObIAllocator &allocator,
                                             const common::ObString &content,
                                             const ObSplitParams &params,
                                             common::ObIArray<ObSplitChunk> &chunks)
{
  int64_t dummy_offset = 0;
  int64_t base_id = 0;
  return split_text_content(allocator, content, params, 0, ObString(), base_id, dummy_offset, chunks);
}

int ObExprAiSplitDocument::split_by_word(common::ObIAllocator &allocator,
                                         const common::ObString &content,
                                         const ObSplitParams &params,
                                         common::ObIArray<ObSplitChunk> &chunks)
{
  ObSplitParams word_params = params;
  word_params.by_word_ = true;
  int64_t dummy_offset = 0;
  int64_t base_id = 0;
  return split_text_content(allocator, content, word_params, 0, ObString(), base_id, dummy_offset, chunks);
}

int ObExprAiSplitDocument::split_markdown_section(common::ObIAllocator &allocator,
                                                  const common::ObString &section_heading,
                                                  const common::ObString &section_body,
                                                  const ObSplitParams &params,
                                                  int64_t section_offset,
                                                  int64_t &base_chunk_id,
                                                  common::ObIArray<ObSplitChunk> &chunks)
{
  int ret = OB_SUCCESS;
  // Build prefix: heading text + newline
  ObString prefix;
  if (section_heading.length() > 0) {
    ObStringBuffer prefix_buf(&allocator);
    if (OB_FAIL(prefix_buf.append(section_heading))) {
      LOG_WARN("failed to append heading", K(ret));
    } else if (OB_FAIL(prefix_buf.append("\n", 1))) {
      LOG_WARN("failed to append newline", K(ret));
    } else {
      prefix = prefix_buf.string();
    }
  }

  if (OB_SUCC(ret)) {
    int64_t next_start = 0;
    if (params.by_word_) {
      ObSplitParams word_params = params;
      word_params.by_word_ = true;
      if (OB_FAIL(split_text_content(allocator, section_body, word_params, section_offset,
                                      prefix, base_chunk_id, next_start, chunks))) {
        LOG_WARN("failed to split markdown section by word", K(ret));
      }
    } else {
      if (OB_FAIL(split_text_content(allocator, section_body, params, section_offset,
                                      prefix, base_chunk_id, next_start, chunks))) {
        LOG_WARN("failed to split markdown section by sentence", K(ret));
      }
    }
    UNUSED(next_start);
  }

  return ret;
}

int ObExprAiSplitDocument::split_markdown(common::ObIAllocator &allocator,
                                          const common::ObString &content,
                                          const ObSplitParams &params,
                                          common::ObIArray<ObSplitChunk> &chunks)
{
  int ret = OB_SUCCESS;
  const char *ptr = content.ptr();
  int64_t len = content.length();
  int64_t base_chunk_id = 0;

  // Find markdown headings (# ...)
  // A heading starts with # at the beginning of a line
  int64_t heading_start = -1;
  int64_t heading_end = -1;
  ObString section_heading;
  int64_t section_content_start = 0;

  for (int64_t i = 0; i < len && OB_SUCC(ret); i++) {
    // Check if this is a heading: '#' at start of line or start of content
    if (ptr[i] == '#' && (i == 0 || ptr[i-1] == '\n')) {
      // Found heading start
      if (heading_start >= 0 && section_content_start < i) {
        // Process previous section
        int64_t section_offset = heading_start;
        ObString section_body(section_content_start - heading_end, 
                              ptr + heading_end);
        if (OB_FAIL(split_markdown_section(allocator, section_heading, section_body,
                                            params, section_offset, base_chunk_id, chunks))) {
          LOG_WARN("failed to split markdown section", K(ret));
        }
      }

      heading_start = i;
      // Find end of heading line
      int64_t j = i;
      while (j < len && ptr[j] != '\n') j++;
      heading_end = j;
      if (j < len) j++; // skip newline
      section_heading = ObString(heading_end - heading_start, ptr + heading_start);
      section_content_start = j;
    }
  }

  // Process last section
  if (OB_SUCC(ret)) {
    if (heading_start >= 0) {
      // There was at least one heading
      if (section_content_start < len) {
        int64_t section_offset = heading_start;
        ObString section_body(len - section_content_start, ptr + section_content_start);
        if (OB_FAIL(split_markdown_section(allocator, section_heading, section_body,
                                            params, section_offset, base_chunk_id, chunks))) {
          LOG_WARN("failed to split last markdown section", K(ret));
        }
      }
    } else {
      // No headings found, treat as plain text
      ObSplitParams text_params = params;
      text_params.type_is_markdown_ = false;
      int64_t dummy_offset = 0;
      if (OB_FAIL(split_text_content(allocator, content, text_params, 0, ObString(),
                                      base_chunk_id, dummy_offset, chunks))) {
        LOG_WARN("failed to split plain text in markdown mode", K(ret));
      }
    }
  }

  return ret;
}

int ObExprAiSplitDocument::split_document(common::ObIAllocator &allocator,
                                          const common::ObString &content,
                                          const common::ObString &params_json,
                                          common::ObIArray<ObSplitChunk> &chunks)
{
  int ret = OB_SUCCESS;
  ObSplitParams params;

  if (OB_FAIL(parse_params(params_json, params))) {
    LOG_WARN("failed to parse params", K(ret));
  } else if (params.type_is_markdown_) {
    if (OB_FAIL(split_markdown(allocator, content, params, chunks))) {
      LOG_WARN("failed to split markdown", K(ret));
    }
  } else {
    if (params.by_word_) {
      if (OB_FAIL(split_by_word(allocator, content, params, chunks))) {
        LOG_WARN("failed to split by word", K(ret));
      }
    } else {
      if (OB_FAIL(split_by_sentence(allocator, content, params, chunks))) {
        LOG_WARN("failed to split by sentence", K(ret));
      }
    }
  }

  return ret;
}

int ObExprAiSplitDocument::eval_ai_split_document(const ObExpr &expr,
                                                  ObEvalCtx &ctx,
                                                  ObDatum &res)
{
  INIT_SUCC(ret);
  ObDatum *arg_content = NULL;
  ObDatum *arg_params = NULL;

  if (expr.arg_cnt_ < 1) {
    ret = OB_ERR_PARAM_SIZE;
    LOG_WARN("ai_split_document requires at least 1 argument", K(ret));
  } else if (OB_FAIL(expr.eval_param_value(ctx, arg_content, arg_params))) {
    // Handle case with only 1 arg
    if (expr.arg_cnt_ == 1) {
      arg_params = NULL;
      if (OB_FAIL(expr.eval_param_value(ctx, arg_content))) {
        LOG_WARN("evaluate parameters failed", K(ret));
      }
    } else {
      LOG_WARN("evaluate parameters failed", K(ret));
    }
  }

  if (OB_FAIL(ret)) {
  } else if (arg_content->is_null()) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("ai_split_document content cannot be null", K(ret));
    LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_split_document, content cannot be null");
    res.set_null();
  } else {
    ObEvalCtx::TempAllocGuard tmp_alloc_g(ctx);
    MultimodeAlloctor temp_allocator(tmp_alloc_g.get_allocator(), expr.type_, ret);
    lib::ObMallocHookAttrGuard malloc_guard(lib::ObMemAttr(N_AI_SPLIT_DOCUMENT));

    ObString content;
    if (OB_FAIL(ObTextStringHelper::read_real_string_data(
        temp_allocator, *arg_content,
        expr.args_[0]->datum_meta_,
        expr.args_[0]->obj_meta_.has_lob_header(),
        content))) {
      LOG_WARN("fail to get real string data", K(ret));
    } else {
      ObString params_json;
      if (expr.arg_cnt_ >= 2 && OB_NOT_NULL(arg_params) && !arg_params->is_null()) {
        if (OB_FAIL(ObTextStringHelper::read_real_string_data(
            temp_allocator, *arg_params,
            expr.args_[1]->datum_meta_,
            expr.args_[1]->obj_meta_.has_lob_header(),
            params_json))) {
          LOG_WARN("fail to get real string data for params", K(ret));
        }
      }

      if (OB_SUCC(ret)) {
        ObSEArray<ObSplitChunk, 16> chunks;
        if (OB_FAIL(split_document(temp_allocator, content, params_json, chunks))) {
          LOG_WARN("failed to split document", K(ret));
        } else {
          // Build JSON array result: [{"chunk_id":0,"chunk_offset":0,"chunk_length":15,"chunk_text":"..."},...]
          // For the function table path, the data will be used by the custom operator
          // For simplicity, build a JSON string representation
          ObJsonBuffer json_buf(&temp_allocator);
          if (OB_FAIL(json_buf.append("["))) {
            LOG_WARN("failed to append [", K(ret));
          }
          for (int64_t i = 0; OB_SUCC(ret) && i < chunks.count(); ++i) {
            if (i > 0) {
              if (OB_FAIL(json_buf.append(","))) {
                LOG_WARN("failed to append ,", K(ret));
              }
            }
            if (OB_FAIL(ret)) break;
            // Build JSON for this chunk
            if (OB_FAIL(json_buf.append("{\"chunk_id\":"))) {
              LOG_WARN("failed to append", K(ret));
            } else if (OB_FAIL(json_buf.append_fmt("%ld", chunks.at(i).chunk_id_))) {
              LOG_WARN("failed to append chunk_id", K(ret));
            } else if (OB_FAIL(json_buf.append(",\"chunk_offset\":"))) {
              LOG_WARN("failed to append", K(ret));
            } else if (OB_FAIL(json_buf.append_fmt("%ld", chunks.at(i).chunk_offset_))) {
              LOG_WARN("failed to append chunk_offset", K(ret));
            } else if (OB_FAIL(json_buf.append(",\"chunk_length\":"))) {
              LOG_WARN("failed to append", K(ret));
            } else if (OB_FAIL(json_buf.append_fmt("%ld", chunks.at(i).chunk_length_))) {
              LOG_WARN("failed to append chunk_length", K(ret));
            } else if (OB_FAIL(json_buf.append(",\"chunk_text\":\""))) {
              LOG_WARN("failed to append", K(ret));
            } else if (OB_FAIL(json_buf.append(chunks.at(i).chunk_text_.ptr(), chunks.at(i).chunk_text_.length()))) {
              LOG_WARN("failed to append chunk_text", K(ret));
            } else if (OB_FAIL(json_buf.append("\"}"))) {
              LOG_WARN("failed to append }", K(ret));
            }
          }
          if (OB_SUCC(ret)) {
            if (OB_FAIL(json_buf.append("]"))) {
              LOG_WARN("failed to append ]", K(ret));
            } else {
              ObString json_result = json_buf.string();
              if (OB_FAIL(ObAIFuncUtils::set_string_result(expr, ctx, res, json_result))) {
                LOG_WARN("failed to set string result", K(ret));
              }
            }
          }
        }
      }
    }
  }
  return ret;
}

int ObExprAiSplitDocument::cg_expr(ObExprCGCtx &expr_cg_ctx,
                                   const ObRawExpr &raw_expr,
                                   ObExpr &rt_expr) const
{
  UNUSED(raw_expr);
  UNUSED(expr_cg_ctx);
  rt_expr.eval_func_ = ObExprAiSplitDocument::eval_ai_split_document;
  return OB_SUCCESS;
}

} // namespace sql
} // namespace oceanbase
