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
#pragma once
#include <random>
#include <algorithm>

#include "td/utils/Status.h"
#include "td/utils/Span.h"
#include "td/utils/Random.h"

namespace td {
namespace online_code {
template <class Random>
class UniformDistributionSimple {
 public:
  UniformDistributionSimple(uint32 L, uint32 R) : L_(L), R_(R) {
  }
  void set_random(Random *random) {
    random_ = random;
  }
  uint32 operator()() {
    return static_cast<uint32>((*random_)() % (R_ - L_ + 1)) + L_;
  }

 private:
  Random *random_{nullptr};
  uint32 L_;
  uint32 R_;
};

using Random = Random::Xorshift128plus;
using UniformDistribution = UniformDistributionSimple<Random>;

class Rfc {
 public:
  class Parameters {
   public:
    Parameters(size_t source_blocks, double epsilon, uint32 quality);
    size_t source_blocks_count() const;
    size_t outer_encoding_blocks_count() const;
    size_t estimated_packets() const;

    template <class F>
    void outer_encoding_for_each(F &&f) const {
      Random r(static_cast<uint32>(1));
      outer_distribution_.set_random(&r);

      for (uint32 j = 0; j < outer_encoding_blocks_count(); j++) {
        f(j, uint32(source_blocks_count() + j));
      }
      for (uint32 i = 0; i < source_blocks_count(); i++) {
        for (uint32 j = 0; j < quality_; j++) {
          f(outer_distribution_(), i);
        }
      }
    }

    Span<uint32> get_inner_encoding_row(size_t row_id) const;

   private:
    struct DegreeDistribution {
     public:
      DegreeDistribution(double epsilon);

      uint32 get_degree(double x) const;

      size_t get_max_degree() const;

     private:
      std::vector<double> p_;
    };

    uint32 source_blocks_;
    double epsilon_;
    uint32 quality_;
    DegreeDistribution degree_distribution_;
    size_t outer_encoding_blocks_count_;
    mutable std::vector<uint32> row_buffer_ = std::vector<uint32>(degree_distribution_.get_max_degree(), 0);

    mutable UniformDistribution outer_distribution_{0, static_cast<uint32>(outer_encoding_blocks_count()) - 1};
    mutable UniformDistribution inner_distribution_{
        0, static_cast<uint32>(outer_encoding_blocks_count() + source_blocks_count()) - 1};
  };
  static Result<Parameters> get_parameters(size_t K);
};
}  // namespace online_code
}  // namespace td
