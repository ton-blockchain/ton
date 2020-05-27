#include "LossSender.h"

#include "td/utils/logging.h"

#if TON_HAVE_GSL
#include <gsl/gsl_cdf.h>
#endif

#include <cmath>

namespace ton {
namespace rldp2 {
namespace {
// works for 1e-x, where x in {1..10}
double ndtri_fast(double p) {
  if (p < 2e-10) {
    return 6.361340902404;
  }
  if (p < 2e-9) {
    return 5.997807015008;
  }
  if (p < 2e-8) {
    return 5.612001244175;
  }
  if (p < 2e-7) {
    return 5.199337582193;
  }
  if (p < 2e-6) {
    return 4.753424308823;
  }
  if (p < 2e-5) {
    return 4.264890793923;
  }
  if (p < 2e-4) {
    return 3.719016485456;
  }
  if (p < 2e-3) {
    return 3.090232306168;
  }
  if (p < 2e-2) {
    return 2.326347874041;
  }
  return 1.281551565545;
}
}  // namespace

LossSender::LossSender(double loss, double p) : loss_(loss), p_(p) {
  v_.resize(2);
  v_[0] = 1;
  res_.push_back(0);
  S_ = ndtri_fast(p_);
  sigma_ = p * (1 - p);
  //LOG(ERROR) << S_ << " " << ndtri(1 - p_);
  //CHECK(fabs(S_ - ndtri(1 - p_)) < 1e-6);
}

int LossSender::send_n(int n) {
  if (n < 50) {
    return send_n_exact(n);
  }
  return send_n_approx_nbd(n);
}

int LossSender::send_n_exact(int n) {
  while ((int)res_.size() <= n) {
    step();
  }
  return res_[n];
}

int LossSender::send_n_approx_norm(int n) {
  double a = (1 - loss_) * (1 - loss_);
  double b = loss_ * (loss_ - 1) * (2 * n + S_ * S_);
  double c = loss_ * loss_ * n * n + S_ * S_ * n * loss_ * (loss_ - 1);
  double x = ((-b + sqrt(b * b - 4 * a * c)) / (2 * a));
  return (int)(x + n + 1);
}

int LossSender::send_n_approx_nbd(int n) {
#if TON_HAVE_GSL
  auto mean = n * loss_ / (1 - loss_);
  auto var = sqrt(mean / (1 - loss_));
  auto min_k = static_cast<int>(mean + var);
  auto max_k = min_k + static_cast<int>(var + 1) * 15;
  while (min_k + 1 < max_k) {
    int k = (min_k + max_k) / 2;
    if (gsl_cdf_negative_binomial_P(k, 1 - loss_, n) > 1 - p_) {
      max_k = k;
    } else {
      min_k = k;
    }
  }
  return max_k + n;
#endif
  return send_n_approx_norm(n);
}

int LossSender::send_n_approx_pd(int n) {
#if TON_HAVE_GSL
  for (int k = 0;; k++) {
    if (gsl_cdf_poisson_P(k, (n + k) * loss_) > 1 - p_) {
      return k + n;
    }
  }
#endif
  return send_n_approx_norm(n);
}
bool LossSender::has_good_approx() {
#if TON_HAVE_GSL
  return true;
#else
  return false;
#endif
}

void LossSender::step() {
  n_++;
  v_.push_back(0);
  v_[n_] = v_[n_ - 1];
  for (int j = n_; j >= 0; j--) {
    v_[j + 1] += v_[j] * loss_;
    v_[j] *= (1 - loss_);
  }

  while (res_i_ < n_ && v_[res_i_] < 1 - p_) {
    res_i_++;
  }
  auto left_ = n_ - res_i_;
  if ((int)res_.size() == left_) {
    res_.push_back(n_);
  }
}

}  // namespace rldp2
}  // namespace ton
