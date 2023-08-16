// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements.  See the NOTICE file
// distributed with this work for additional information
// regarding copyright ownership.  The ASF licenses this file
// to you under the Apache License, Version 2.0 (the
// "License"); you may not use this file except in compliance
// with the License.  You may obtain a copy of the License at
//
//   http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing,
// software distributed under the License is distributed on an
// "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
// KIND, either express or implied.  See the License for the
// specific language governing permissions and limitations
// under the License.

#include "vec/json/parse2column.h"

#include <assert.h>
#include <fmt/format.h>
#include <glog/logging.h>
#include <parallel_hashmap/phmap.h>
#include <simdjson/simdjson.h> // IWYU pragma: keep
#include <stddef.h>

#include <algorithm>
#include <memory>
#include <mutex>
#include <optional>
#include <ostream>
#include <stack>
#include <string>
#include <string_view>
#include <utility>

#include "common/config.h"
#include "common/exception.h"
#include "common/status.h"
#include "vec/columns/column.h"
#include "vec/columns/column_object.h"
#include "vec/columns/column_string.h"
#include "vec/common/assert_cast.h"
#include "vec/common/field_visitors.h"
#include "vec/common/schema_util.h"
#include "vec/common/string_ref.h"
#include "vec/core/field.h"
#include "vec/data_types/data_type.h"
#include "vec/json/json_parser.h"
#include "vec/json/path_in_data.h"
#include "vec/json/simd_json_parser.h"

namespace doris::vectorized {

/** Pool for objects that cannot be used from different threads simultaneously.
  * Allows to create an object for each thread.
  * Pool has unbounded size and objects are not destroyed before destruction of pool.
  *
  * Use it in cases when thread local storage is not appropriate
  *  (when maximum number of simultaneously used objects is less
  *   than number of running/sleeping threads, that has ever used object,
  *   and creation/destruction of objects is expensive).
  */
template <typename T>
class SimpleObjectPool {
protected:
    /// Hold all available objects in stack.
    std::mutex mutex;
    std::stack<std::unique_ptr<T>> stack;
    /// Specialized deleter for std::unique_ptr.
    /// Returns underlying pointer back to stack thus reclaiming its ownership.
    struct Deleter {
        SimpleObjectPool<T>* parent;
        Deleter(SimpleObjectPool<T>* parent_ = nullptr) : parent {parent_} {} /// NOLINT
        void operator()(T* owning_ptr) const {
            std::lock_guard lock {parent->mutex};
            parent->stack.emplace(owning_ptr);
        }
    };

public:
    using Pointer = std::unique_ptr<T, Deleter>;
    /// Extracts and returns a pointer from the stack if it's not empty,
    ///  creates a new one by calling provided f() otherwise.
    template <typename Factory>
    Pointer get(Factory&& f) {
        std::unique_lock lock(mutex);
        if (stack.empty()) {
            return {f(), this};
        }
        auto object = stack.top().release();
        stack.pop();
        return std::unique_ptr<T, Deleter>(object, Deleter(this));
    }
    /// Like get(), but creates object using default constructor.
    Pointer getDefault() {
        return get([] { return new T; });
    }
};

SimpleObjectPool<JsonParser> parsers_pool;

using Node = typename ColumnObject::Subcolumns::Node;
/// Visitor that keeps @num_dimensions_to_keep dimensions in arrays
/// and replaces all scalars or nested arrays to @replacement at that level.
class FieldVisitorReplaceScalars : public StaticVisitor<Field> {
public:
    FieldVisitorReplaceScalars(const Field& replacement_, size_t num_dimensions_to_keep_)
            : replacement(replacement_), num_dimensions_to_keep(num_dimensions_to_keep_) {}
    template <typename T>
    Field operator()(const T& x) const {
        if constexpr (std::is_same_v<T, Array>) {
            if (num_dimensions_to_keep == 0) {
                return replacement;
            }
            const size_t size = x.size();
            Array res(size);
            for (size_t i = 0; i < size; ++i) {
                res[i] = apply_visitor(
                        FieldVisitorReplaceScalars(replacement, num_dimensions_to_keep - 1), x[i]);
            }
            return res;
        } else {
            return replacement;
        }
    }

private:
    const Field& replacement;
    size_t num_dimensions_to_keep;
};

template <typename ParserImpl>
void parse_json_to_variant(IColumn& column, const char* src, size_t length,
                           JSONDataParser<ParserImpl>* parser) {
    auto& column_object = assert_cast<ColumnObject&>(column);
    std::optional<ParseResult> result;
    /// Treat empty string as an empty object
    /// for better CAST from String to Object.
    if (length > 0) {
        result = parser->parse(src, length);
    } else {
        result = ParseResult {};
    }
    if (!result) {
        LOG(INFO) << "failed to parse " << std::string_view(src, length) << ", length= " << length;
        throw doris::Exception(ErrorCode::INVALID_ARGUMENT, "Failed to parse object {}",
                               std::string_view(src, length));
    }
    auto& [paths, values] = *result;
    assert(paths.size() == values.size());
    phmap::flat_hash_set<StringRef, StringRefHash> paths_set;
    size_t num_rows = column_object.size();
    for (size_t i = 0; i < paths.size(); ++i) {
        FieldInfo field_info;
        get_field_info(values[i], &field_info);
        if (is_nothing(field_info.scalar_type)) {
            continue;
        }
        if (!paths_set.insert(paths[i].get_path()).second) {
            // return Status::DataQualityError(
            //         fmt::format("Object has ambiguous path {}, {}", paths[i].get_path()));
            throw doris::Exception(ErrorCode::INVALID_ARGUMENT, "Object has ambiguous path {}",
                                   paths[i].get_path());
        }

        if (!column_object.has_subcolumn(paths[i])) {
            column_object.add_sub_column(paths[i], num_rows);
        }
        auto* subcolumn = column_object.get_subcolumn(paths[i]);
        if (!subcolumn) {
            throw doris::Exception(ErrorCode::INVALID_ARGUMENT, "Failed to find sub column {}",
                                   paths[i].get_path());
        }
        assert(subcolumn->size() == num_rows);
        subcolumn->insert(std::move(values[i]), std::move(field_info));
    }
    // /// Insert default values to missed subcolumns.
    const auto& subcolumns = column_object.get_subcolumns();
    for (const auto& entry : subcolumns) {
        if (!paths_set.contains(entry->path.get_path())) {
            entry->data.insertDefault();
        }
    }
    column_object.incr_num_rows();
}

Status parse_json_to_variant(IColumn& column, const ColumnString& raw_json_column,
                             double max_filter_ratio, IColumn::Filter& filter) {
    auto parser = parsers_pool.get([] { return new JsonParser(); });
    int32_t parse_failed = 0;
    for (size_t i = 0; i < raw_json_column.size(); ++i) {
        StringRef raw_json = raw_json_column.get_data_at(i);
        try {
            parse_json_to_variant(column, raw_json.data, raw_json.size, parser.get());
            filter[i] = 1;
        } catch (const doris::Exception& e) {
            double failed_ratio = static_cast<double>(parse_failed) / raw_json_column.size();
            if (e.code() == ErrorCode::INVALID_ARGUMENT && failed_ratio < max_filter_ratio) {
                filter[i] = 0;
                continue;
            }
            return Status::Aborted("Too many filtered rows {}, {}", parse_failed,
                                   raw_json_column.size());
        }
    }
    return Status::OK();
}

} // namespace doris::vectorized
