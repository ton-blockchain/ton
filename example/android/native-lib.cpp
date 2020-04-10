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
#include <jni.h>
#include <string>
#include <cstdint>
#include <cstdlib>
#include <utility>

#include "tl/tl_jni_object.h"

#include "tonlib/tonlib_client_json.h"

#include "tonlib/Client.h"
#include "auto/tl/tonlib_api.h"

extern "C" JNIEXPORT jstring JNICALL Java_drinkless_org_tonlib_MainActivity_stringFromJNI(JNIEnv *env,
                                                                                          jobject /* this */,
                                                                                          jstring dir) {
  std::string hello = "Hello from C++";
  std::string query = "{\"@type\": \"runTests\", \"dir\":\"";
  query += env->GetStringUTFChars(dir, 0);
  query += "\"}";
  return env->NewStringUTF(tonlib_client_json_execute(nullptr, query.c_str()));

  //return env->NewStringUTF(hello.c_str());
}

extern "C" JNIEXPORT jlong JNICALL Java_drinkless_org_tonlib_ClientJsonNative_create(JNIEnv *env, jobject /* this */) {
  return reinterpret_cast<jlong>(tonlib_client_json_create());
}
extern "C" JNIEXPORT void JNICALL Java_drinkless_org_tonlib_ClientJsonNative_send(JNIEnv *env, jobject /* this */,
                                                                                  jlong client, jstring j_query) {
  auto query = td::jni::from_jstring(env, j_query);
  return tonlib_client_json_send(reinterpret_cast<void *>(client), query.c_str());
}
extern "C" JNIEXPORT jstring JNICALL Java_drinkless_org_tonlib_ClientJsonNative_execute(JNIEnv *env, jobject /* this */,
                                                                                        jstring j_query) {
  auto query = td::jni::from_jstring(env, j_query);
  return td::jni::to_jstring(env, tonlib_client_json_execute(nullptr, query.c_str()));
}
extern "C" JNIEXPORT jstring JNICALL Java_drinkless_org_tonlib_ClientJsonNative_receive(JNIEnv *env, jobject /* this */,
                                                                                        jlong client, jdouble timeout) {
  return td::jni::to_jstring(env, tonlib_client_json_receive(reinterpret_cast<void *>(client), timeout));
}
extern "C" JNIEXPORT void JNICALL Java_drinkless_org_tonlib_ClientJsonNative_destroy(JNIEnv *env, jobject /* this */,
                                                                                     jlong client) {
  return tonlib_client_json_destroy(reinterpret_cast<void *>(client));
}

// ---

namespace td_jni {

static tonlib_api::object_ptr<tonlib_api::Function> fetch_function(JNIEnv *env, jobject function) {
  td::jni::reset_parse_error();
  auto result = tonlib_api::Function::fetch(env, function);
  if (td::jni::have_parse_error()) {
    std::abort();
  }
  return result;
}

static tonlib::Client *get_client(jlong client_id) {
  return reinterpret_cast<tonlib::Client *>(static_cast<std::uintptr_t>(client_id));
}

static jlong Client_createNativeClient(JNIEnv *env, jclass clazz) {
  return static_cast<jlong>(reinterpret_cast<std::uintptr_t>(new tonlib::Client()));
}

static void Client_nativeClientSend(JNIEnv *env, jclass clazz, jlong client_id, jlong id, jobject function) {
  get_client(client_id)->send({static_cast<std::uint64_t>(id), fetch_function(env, function)});
}

static jint Client_nativeClientReceive(JNIEnv *env, jclass clazz, jlong client_id, jlongArray ids, jobjectArray events,
                                       jdouble timeout) {
  auto client = get_client(client_id);
  jsize events_size = env->GetArrayLength(ids);  // ids and events size must be of equal size
  if (events_size == 0) {
    return 0;
  }
  jsize result_size = 0;

  auto response = client->receive(timeout);
  while (response.object) {
    jlong result_id = static_cast<jlong>(response.id);
    env->SetLongArrayRegion(ids, result_size, 1, &result_id);

    jobject object;
    response.object->store(env, object);
    env->SetObjectArrayElement(events, result_size, object);
    env->DeleteLocalRef(object);

    result_size++;
    if (result_size == events_size) {
      break;
    }

    response = client->receive(0);
  }
  return result_size;
}

static jobject Client_nativeClientExecute(JNIEnv *env, jclass clazz, jobject function) {
  jobject result;
  tonlib::Client::execute({0, fetch_function(env, function)}).object->store(env, result);
  return result;
}

static void Client_destroyNativeClient(JNIEnv *env, jclass clazz, jlong client_id) {
  delete get_client(client_id);
}

static jstring Object_toString(JNIEnv *env, jobject object) {
  return td::jni::to_jstring(env, to_string(tonlib_api::Object::fetch(env, object)));
}

static jstring Function_toString(JNIEnv *env, jobject object) {
  return td::jni::to_jstring(env, to_string(tonlib_api::Function::fetch(env, object)));
}

static constexpr jint JAVA_VERSION = JNI_VERSION_1_6;
static JavaVM *java_vm;
static jclass log_class;

static void on_fatal_error(const char *error_message) {
  auto env = td::jni::get_jni_env(java_vm, JAVA_VERSION);
  jmethodID on_fatal_error_method = env->GetStaticMethodID(log_class, "onFatalError", "(Ljava/lang/String;)V");
  if (env && on_fatal_error_method) {
    jstring error_str = td::jni::to_jstring(env.get(), error_message);
    env->CallStaticVoidMethod(log_class, on_fatal_error_method, error_str);
    if (error_str) {
      env->DeleteLocalRef(error_str);
    }
  }
}

static jint register_native(JavaVM *vm) {
  JNIEnv *env;
  if (vm->GetEnv(reinterpret_cast<void **>(&env), JAVA_VERSION) != JNI_OK) {
    return -1;
  }

  java_vm = vm;

  auto register_method = [env](jclass clazz, std::string name, std::string signature, auto function_ptr) {
    td::jni::register_native_method(env, clazz, std::move(name), std::move(signature),
                                    reinterpret_cast<void *>(function_ptr));
  };

  auto client_class = td::jni::get_jclass(env, PACKAGE_NAME "/Client");
  //log_class = td::jni::get_jclass(env, PACKAGE_NAME "/Log");
  auto object_class = td::jni::get_jclass(env, PACKAGE_NAME "/TonApi$Object");
  auto function_class = td::jni::get_jclass(env, PACKAGE_NAME "/TonApi$Function");

#define TD_OBJECT "L" PACKAGE_NAME "/TonApi$Object;"
#define TD_FUNCTION "L" PACKAGE_NAME "/TonApi$Function;"
  register_method(client_class, "createNativeClient", "()J", Client_createNativeClient);
  register_method(client_class, "nativeClientSend", "(JJ" TD_FUNCTION ")V", Client_nativeClientSend);
  register_method(client_class, "nativeClientReceive", "(J[J[" TD_OBJECT "D)I", Client_nativeClientReceive);
  register_method(client_class, "nativeClientExecute", "(" TD_FUNCTION ")" TD_OBJECT, Client_nativeClientExecute);
  register_method(client_class, "destroyNativeClient", "(J)V", Client_destroyNativeClient);

  register_method(object_class, "toString", "()Ljava/lang/String;", Object_toString);
  register_method(function_class, "toString", "()Ljava/lang/String;", Function_toString);
#undef TD_FUNCTION
#undef TD_OBJECT

  td::jni::init_vars(env, PACKAGE_NAME);
  tonlib_api::Object::init_jni_vars(env, PACKAGE_NAME);
  tonlib_api::Function::init_jni_vars(env, PACKAGE_NAME);
  // FIXME
  //td::Log::set_fatal_error_callback(on_fatal_error);

  return JAVA_VERSION;
}

}  // namespace td_jni

JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM *vm, void *reserved) {
  static jint jni_version = td_jni::register_native(vm);  // call_once
  return jni_version;
}
