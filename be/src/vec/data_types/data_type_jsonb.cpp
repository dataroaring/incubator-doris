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

#include "data_type_jsonb.h"

#include <typeinfo>
#include <utility>

#include "util/jsonb_utils.h"
#include "vec/columns/column_const.h"
#include "vec/common/assert_cast.h"
#include "vec/common/string_buffer.hpp"
#include "vec/common/string_ref.h"
#include "vec/io/reader_buffer.h"

namespace doris {
namespace vectorized {
class IColumn;
} // namespace vectorized
} // namespace doris

namespace doris::vectorized {

std::string DataTypeJsonb::to_string(const IColumn& column, size_t row_num) const {
    auto result = check_column_const_set_readability(column, row_num);
    ColumnPtr ptr = result.first;
    row_num = result.second;

    const StringRef& s = assert_cast<const ColumnString&>(*ptr).get_data_at(row_num);
    return s.size > 0 ? JsonbToJson::jsonb_to_json_string(s.data, s.size) : "";
}

void DataTypeJsonb::to_string(const class doris::vectorized::IColumn& column, size_t row_num,
                              class doris::vectorized::BufferWritable& ostr) const {
    auto result = check_column_const_set_readability(column, row_num);
    ColumnPtr ptr = result.first;
    row_num = result.second;

    const StringRef& s = assert_cast<const ColumnString&>(*ptr).get_data_at(row_num);
    if (s.size > 0) {
        std::string str = JsonbToJson::jsonb_to_json_string(s.data, s.size);
        ostr.write(str.c_str(), str.size());
    }
}

Status DataTypeJsonb::from_string(ReadBuffer& rb, IColumn* column) const {
    JsonBinaryValue value;
    RETURN_IF_ERROR(value.from_json_string(rb.position(), rb.count()));

    auto* column_string = static_cast<ColumnString*>(column);
    column_string->insert_data(value.value(), value.size());

    return Status::OK();
}

MutableColumnPtr DataTypeJsonb::create_column() const {
    return ColumnString::create();
}

bool DataTypeJsonb::equals(const IDataType& rhs) const {
    return typeid(rhs) == typeid(*this);
}

int64_t DataTypeJsonb::get_uncompressed_serialized_bytes(const IColumn& column,
                                                         int data_version) const {
    return data_type_string.get_uncompressed_serialized_bytes(column, data_version);
}

char* DataTypeJsonb::serialize(const IColumn& column, char* buf, int data_version) const {
    return data_type_string.serialize(column, buf, data_version);
}

const char* DataTypeJsonb::deserialize(const char* buf, MutableColumnPtr* column,
                                       int data_version) const {
    return data_type_string.deserialize(buf, column, data_version);
}

} // namespace doris::vectorized
