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

    Copyright 2017-2020 Telegram Systems LLP
*/
#include "td/fec/online/Rfc.h"
namespace td {
namespace online_code {
Result<Rfc::Parameters> Rfc::get_parameters(size_t K) {
  return Rfc::Parameters(K, 0.001, 3);
}
Rfc::Parameters::Parameters(size_t source_blocks, double epsilon, uint32 quality)
    : source_blocks_(static_cast<uint32>(source_blocks))
    , epsilon_(epsilon)
    , quality_(quality)
    , degree_distribution_{epsilon}
    , outer_encoding_blocks_count_{
          static_cast<uint32>(1 + 0.55 * quality * epsilon * static_cast<double>(source_blocks))} {
}
size_t Rfc::Parameters::source_blocks_count() const {
  return source_blocks_;
}
size_t Rfc::Parameters::outer_encoding_blocks_count() const {
  return outer_encoding_blocks_count_;
}
size_t Rfc::Parameters::estimated_packets() const {
  return size_t(static_cast<double>(source_blocks_count() + outer_encoding_blocks_count()) * (1 + epsilon_));
}
Span<uint32> Rfc::Parameters::get_inner_encoding_row(size_t row_id) const {
  Random r(static_cast<uint32>(row_id));
  uint32 degree = degree_distribution_.get_degree(static_cast<uint32>(r() % (1 << 20)) * 1.0 / (1 << 20));
  inner_distribution_.set_random(&r);
  MutableSpan<uint32> res(row_buffer_.data(), degree);
  for (auto &x : res) {
    x = inner_distribution_();
  }
  //std::sort(res.begin(), res.end());
  //res.truncate(std::unique(res.begin(), res.end()) - res.begin());
  return res;
}
Rfc::Parameters::DegreeDistribution::DegreeDistribution(double epsilon) {
  uint32 F = static_cast<uint32>(log(epsilon * epsilon / 4) / log(1 - epsilon / 2) + 1 - 1e-9);
  double x = 1 - (1 + 1.0 / F) / (1 + epsilon);
  p_.reserve(F);
  p_.push_back(x);
  for (uint32 i = 2; i <= F; i++) {
    auto y = (1 - x) * F / ((F - 1.0) * i * (i - 1));
    p_.push_back(p_.back() + y);
  }
}

uint32 Rfc::Parameters::DegreeDistribution::get_degree(double x) const {
  for (uint32 i = 0; i < p_.size(); i++) {
    if (x < p_[i]) {
      return i + 1;
    }
  }
  UNREACHABLE();
  return 1;
}

size_t Rfc::Parameters::DegreeDistribution::get_max_degree() const {
  return p_.size();
}
}  // namespace online_code
}  // namespace td
