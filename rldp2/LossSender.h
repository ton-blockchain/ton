#pragma once
#include <vector>

namespace ton {
namespace rldp2 {
struct LossSender {
  LossSender(double loss, double p);
  int send_n(int n);

  int send_n_exact(int n);
  int send_n_approx_norm(int n);
  int send_n_approx_nbd(int n);
  int send_n_approx_pd(int n);

  bool has_good_approx();

 private:
  double loss_;
  double p_;
  double S_;
  double sigma_;
  int n_{0};
  std::vector<double> v_;
  std::vector<int> res_;
  int res_i_{0};

  void step();
};

}  // namespace rldp2
}  // namespace ton
