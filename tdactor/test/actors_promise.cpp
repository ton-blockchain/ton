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
#include "td/actor/MultiPromise.h"
#include "td/actor/PromiseFuture.h"
#include "td/actor/actor.h"
#include "td/utils/MovableValue.h"
#include "td/utils/tests.h"

template <class T>
class X {
 public:
  X() = default;
  X(X &&) = default;
  template <class S>
  X(S s) : t(s) {
  }
  T t;
};

TEST(Actor, promise) {
  using Int = td::MovableValue<int>;
  using td::Promise;
  using td::Result;

  auto set_result_int = [](Result<Int> &destination) {
    return [&destination](Result<Int> value) { destination = std::move(value); };
  };

  {
    Result<Int> result{2};
    {
      Promise<Int> promise = set_result_int(result);
      promise.set_value(Int{3});
    }
    ASSERT_TRUE(result.is_ok());
    ASSERT_EQ(result.ok().get(), 3);
  }

  {
    Result<Int> result{2};
    {
      Promise<Int> promise = set_result_int(result);
      (void)promise;
      // will set Status::Error() on destruction
    }
    ASSERT_TRUE(result.is_error());
  }

  {
    std::unique_ptr<int> res;
    Promise<td::Unit> x = [a = std::make_unique<int>(5), &res](td::Result<>) mutable { res = std::move(a); };
    x.set_value({});
    CHECK(*res == 5);
  }

  {  //{
    //Promise<Int> promise;
    //std::tuple<Promise<Int> &&> f(std::move(promise));
    //std::tuple<Promise<Int>> x = std::move(f);
    //}

    {
      //using T = Result<int>;
      //using T = std::unique_ptr<int>;
      //using T = std::function<int()>;
      //using T = std::vector<int>;
      //using T = X<int>;
      ////using T = Promise<Int>;
      //T f;
      //std::tuple<T &&> g(std::move(f));
      //std::tuple<T> h = std::move(g);
    }
  }

  {
    int result = 0;
    // MSVC doesn't like this particular instance of lambda_promise and I personally can't care less.
#if !TD_MSVC
    auto promise = td::lambda_promise<int>([&](auto x) { result = x.move_as_ok(); });
    promise.set_value(5);
    ASSERT_EQ(5, result);
#endif

    Promise<int> promise2 = [&](auto x) { result = x.move_as_ok(); };
    promise2.set_value(6);
    ASSERT_EQ(6, result);
  }
}

TEST(Actor, safe_promise) {
  int res = 0;
  {
    td::Promise<int> promise = td::PromiseCreator::lambda([&](td::Result<int> x) { res = x.move_as_ok(); });
    auto safe_promise = td::SafePromise<int>(std::move(promise), 2);
    promise = std::move(safe_promise);
    ASSERT_EQ(res, 0);
    auto safe_promise2 = td::SafePromise<int>(std::move(promise), 3);
  }
  ASSERT_EQ(res, 3);
}

TEST(Actor, split_promise) {
  using td::Promise;
  using td::Result;
  using td::split_promise;
  using td::SplitPromise;
  {
    td::optional<std::pair<int, double>> x;
    auto pair = [&](Result<std::pair<int, double>> res) { x = res.move_as_ok(); };
    static_assert(std::is_same<SplitPromise<decltype(pair)>::ArgT, std::pair<int, double>>::value, "A");
    static_assert(
        std::is_same<SplitPromise<decltype(pair)>::SplittedT, std::pair<Promise<int>, Promise<double>>>::value, "A");
    auto splitted = split_promise(pair);
    static_assert(std::is_same<decltype(splitted), std::pair<Promise<int>, Promise<double>>>::value, "A");

    splitted.first.set_value(1);
    splitted.second.set_value(2.0);
    CHECK(x.unwrap() == std::make_pair(1, 2.0));
  }  // namespace td
  {
    td::optional<std::tuple<int, double, std::string>> x;
    auto triple = [&](Result<std::tuple<int, double, std::string>> res) { x = res.move_as_ok(); };
    static_assert(std::is_same<SplitPromise<decltype(triple)>::ArgT, std::tuple<int, double, std::string>>::value, "A");
    static_assert(std::is_same<SplitPromise<decltype(triple)>::SplittedT,
                               std::tuple<Promise<int>, Promise<double>, Promise<std::string>>>::value,
                  "A");
    auto splitted = split_promise(triple);
    static_assert(
        std::is_same<decltype(splitted), std::tuple<Promise<int>, Promise<double>, Promise<std::string>>>::value, "A");
    std::get<0>(splitted).set_value(1);
    std::get<1>(splitted).set_value(2.0);
    std::get<2>(splitted).set_value("hello");
    CHECK(x.unwrap() == std::make_tuple(1, 2.0, "hello"));
  }
  {
    int code = 0;
    auto pair = [&](Result<std::pair<int, double>> res) {
      res.ensure_error();
      code = res.error().code();
    };
    auto splitted = split_promise(td::Promise<std::pair<int, double>>(pair));
    splitted.second.set_error(td::Status::Error(123, "123"));
    CHECK(code == 0);
    splitted.first.set_value(1);
    CHECK(code == 123);
  }
}

TEST(Actor, promise_future) {
  using td::make_promise_future;
  {
    auto pf = make_promise_future<int>();
    td::optional<int> res;
    pf.second.map([](int x) { return x * 2; }).map([](int x) { return x + 10; }).map([&](int x) {
      res = x;
      return td::Unit();
    });
    CHECK(!res);
    pf.first.set_value(6);
    ASSERT_EQ(22, res.unwrap());
  }
  {
    LOG(ERROR) << "Second test";
    td::optional<int> res;
    td::make_future(6)
        .map([](int x) { return x * 2; })
        .map([](int x) { return x + 10; })
        .fmap([&](int x) { return td::make_future(x * 2); })
        .finish([&](td::Result<int> x) { res = x.move_as_ok(); });
    ASSERT_EQ(44, res.unwrap());
  }
}

TEST(Actor2, actor_lost_promise) {
  using namespace td::actor;
  using namespace td;
  Scheduler scheduler({1}, false, Scheduler::Paused);

  auto watcher = td::create_shared_destructor([] {
    LOG(ERROR) << "STOP";
    SchedulerContext::get().stop();
  });
  scheduler.run_in_context([watcher = std::move(watcher)] {
    class B : public Actor {
     public:
      void start_up() override {
        stop();
      }
      uint32 query(uint32 x) {
        return x * x;
      }
    };
    class A : public Actor {
     public:
      A(std::shared_ptr<td::Destructor> watcher) : watcher_(std::move(watcher)) {
      }
      void start_up() {
        b_ = create_actor<B>(ActorOptions().with_name("B"));
        //send_closure(b_, &B::query, 2, [self = actor_id(this)](uint32 y) { send_closure(self, &A::on_result, 2, y); });
        send_closure_later(b_, &B::query, 2,
                           [self = actor_id(this), a = std::make_unique<int>()](Result<uint32> y) mutable {
                             LOG(ERROR) << "!";
                             CHECK(y.is_error());
                             send_closure(self, &A::finish);
                           });
        send_closure(b_, &B::query, 2, [self = actor_id(this), a = std::make_unique<int>()](Result<uint32> y) mutable {
          LOG(ERROR) << "!";
          CHECK(y.is_error());
          send_closure(self, &A::finish);
        });
      }
      void finish() {
        LOG(ERROR) << "FINISH";
        stop();
      }

     private:
      std::shared_ptr<td::Destructor> watcher_;
      td::actor::ActorOwn<B> b_;
    };
    create_actor<A>(ActorOptions().with_name("A").with_poll(), watcher).release();
  });
  scheduler.run();
}

TEST(Actor2, MultiPromise) {
  using namespace td;
  MultiPromise::Options fail_on_error;
  fail_on_error.ignore_errors = false;
  MultiPromise::Options ignore_errors;
  ignore_errors.ignore_errors = true;

  std::string str;
  auto log = [&](Result<Unit> res) {
    if (res.is_ok()) {
      str += "OK;";
    } else {
      str += PSTRING() << "E" << res.error().code() << ";";
    }
  };
  auto clear = [&] { str = ""; };

  {
    clear();
    MultiPromise mp(ignore_errors);
    {
      auto mp_init = mp.init_guard();
      mp_init.add_promise(log);
      ASSERT_EQ("", str);
    }
    ASSERT_EQ("OK;", str);
  }

  {
    clear();
    MultiPromise mp(ignore_errors);
    {
      auto mp_init = mp.init_guard();
      mp_init.add_promise(log);
      mp_init.get_promise().set_error(Status::Error(1));
      ASSERT_EQ("", str);
    }
    ASSERT_EQ("OK;", str);
  }

  {
    clear();
    MultiPromise mp(ignore_errors);
    Promise<> promise;
    {
      auto mp_init = mp.init_guard();
      mp_init.add_promise(log);
      promise = mp_init.get_promise();
    }
    ASSERT_EQ("", str);
    {
      auto mp_init = mp.add_promise_or_init(log);
      ASSERT_TRUE(!mp_init);
    }
    promise.set_error(Status::Error(2));
    ASSERT_EQ("OK;OK;", str);
    clear();
    {
      auto mp_init = mp.add_promise_or_init(log);
      ASSERT_TRUE(mp_init);
      ASSERT_EQ("", str);
    }
    ASSERT_EQ("OK;", str);
  }

  {
    clear();
    MultiPromise mp(fail_on_error);
    {
      auto mp_init = mp.init_guard();
      mp_init.get_promise().set_value(Unit());
      mp_init.add_promise(log);
      ASSERT_EQ("", str);
    }
    ASSERT_EQ("OK;", str);
  }

  {
    clear();
    MultiPromise mp(fail_on_error);
    {
      auto mp_init = mp.init_guard();
      mp_init.get_promise().set_value(Unit());
      mp_init.add_promise(log);
      mp_init.get_promise().set_error(Status::Error(1));
      ASSERT_EQ("E1;", str);
      clear();
      mp_init.get_promise().set_error(Status::Error(2));
      ASSERT_EQ("", str);
      mp_init.add_promise(log);
      ASSERT_EQ("E1;", str);
    }
    ASSERT_EQ("E1;", str);
  }

  {
    clear();
    MultiPromise mp(fail_on_error);
    Promise<> promise;
    {
      auto mp_init = mp.init_guard();
      mp_init.get_promise().set_value(Unit());
      mp_init.add_promise(log);
      promise = mp_init.get_promise();
    }
    ASSERT_EQ("", str);
    {
      auto mp_init = mp.add_promise_or_init(log);
      ASSERT_TRUE(mp_init.empty());
    }
    promise.set_error(Status::Error(2));
    ASSERT_EQ("E2;E2;", str);
    clear();

    {
      auto mp_init = mp.add_promise_or_init(log);
      ASSERT_TRUE(!mp_init.empty());
    }
    ASSERT_EQ("OK;", str);
  }
}
