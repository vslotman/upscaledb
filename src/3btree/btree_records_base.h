/*
 * Copyright (C) 2005-2014 Christoph Rupp (chris@crupp.de).
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

/*
 * Base class for RecordLists
 *
 * @exception_safe: unknown
 * @thread_safe: unknown
 */

#ifndef HAM_BTREE_RECORDS_BASE_H
#define HAM_BTREE_RECORDS_BASE_H

#include "0root/root.h"

// Always verify that a file of level N does not include headers > N!

#ifndef HAM_ROOT_H
#  error "root.h was not included"
#endif

namespace hamsterdb {

class BaseRecordList
{
  public:
    BaseRecordList()
      : m_range_size(0) {
    }

    // Checks the integrity of this node. Throws an exception if there is a
    // violation.
    void check_integrity(ham_u32_t count, bool quick = false) const {
    }

    // Rearranges the list
    void vacuumize(ham_u32_t node_count, bool force) const {
    }

  protected:
    // The size of the range (in bytes)
    size_t m_range_size;
};

} // namespace hamsterdb

#endif /* HAM_BTREE_RECORDS_BASE_H */
