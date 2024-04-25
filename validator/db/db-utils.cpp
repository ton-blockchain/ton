/*
    This file is part of TON Blockchain Library.

    TON Blockchain Library is free software: you can redistribute it and/or modify
    it under the terms of the GNU Lesser General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.

    TON Blockchain Library is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU Lesser General Public License for more details.

    You should have received a copy of the GNU Lesser General Public License
    along with TON Blockchain Library.  If not, see <http://www.gnu.org/licenses/>.
*/
#include "db-utils.h"

#include "td/utils/logging.h"

#include <cmath>

namespace ton::validator {

void PercentileStats::insert(double value) {
  values_.insert(value);
}

std::string PercentileStats::to_string() const {
  double percentiles[4] = {0.0, 0.0, 0.0, 0.0};
  double sum = 0.0;
  size_t size = values_.size();
  if (!values_.empty()) {
    size_t indices[4] = {(size_t)std::ceil(0.5 * (double)size) - 1, (size_t)std::ceil(0.95 * (double)size) - 1,
                         (size_t)std::ceil(0.99 * (double)size) - 1, size - 1};
    size_t i = 0;
    for (auto it = values_.begin(); it != values_.end(); ++it, ++i) {
      for (size_t j = 0; j < 4; ++j) {
        if (indices[j] == i) {
          percentiles[j] = *it;
        }
      }
      sum += *it;
    }
  }
  return PSTRING() << "P50 : " << percentiles[0] << " P95 : " << percentiles[1] << " P99 : " << percentiles[2]
                   << " P100 : " << percentiles[3] << " COUNT : " << size << " SUM : " << sum;
}

void PercentileStats::clear() {
  values_.clear();
}

}  // namespace ton::validator