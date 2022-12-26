extern "C" {
  #include <lua.h>
  #include <lauxlib.h>
}
#include <jni.h>
#include <vector>
#include <algorithm>
#include <string>
#include <stdexcept>

static const char key = 0;

inline namespace {
  static int start_jvm(lua_State *L) {
    try {
      if (!lua_isstring(L, 1)) {
        throw std::runtime_error("invalid jar path");
      }
      const char *jar_path = lua_tostring(L, 1);
      JavaVM *vm;
      JNIEnv *env;
      JavaVMInitArgs vm_args;
      JavaVMOption options[1];
      vm_args.version = JNI_VERSION_1_8;
      vm_args.ignoreUnrecognized = JNI_FALSE;
      vm_args.nOptions = sizeof(options) / sizeof(JavaVMOption);
      vm_args.options = options;
      std::string option = "-Djava.class.path=";
      option += jar_path;
      options[0].optionString = const_cast<char *>(option.c_str());
      jint res = JNI_CreateJavaVM(&vm, (void **)&env, &vm_args);
      if (res != JNI_OK) {
        throw std::runtime_error("Failed to create Java VM");
      }
      lua_pushlightuserdata(L, (void *)&key);
      lua_pushlightuserdata(L, (void *)vm);
      lua_settable(L, LUA_REGISTRYINDEX);
      return 0;
    } catch (std::exception &e) {
      lua_settop(L, 0);
      lua_pushnil(L);
      lua_pushstring(L, e.what());
      return 2;
    }
  }
  JavaVM *get_jvm(lua_State *L) {
    JavaVM *vm;
    lua_pushlightuserdata(L, (void *)&key);
    lua_gettable(L, LUA_REGISTRYINDEX);
    if (lua_isnil(L, -1)) {
      throw std::runtime_error("Java VM not started");
    } else {
      vm = (JavaVM *)lua_touserdata(L, -1);
      lua_pop(L, 1);
    }
    return vm;
  }
  struct auto_detach_env {
    JNIEnv *env;
    auto_detach_env(JNIEnv *env) : env(env) {}
    ~auto_detach_env() {
      JavaVM *vm;
      env->GetJavaVM(&vm);
      vm->DetachCurrentThread();
    }
    operator JNIEnv *() const {
      return env;
    }
    JNIEnv *operator->() const {
      return env;
    }
  };
  auto_detach_env get_jvm_env(lua_State *L) {
    JNIEnv *env;
    JavaVM *vm = get_jvm(L);
    jint res = vm->AttachCurrentThread((void **)&env, NULL);
    if (res != JNI_OK) {
      throw std::runtime_error("Failed to attach to Java VM");
    }
    return env;
  }
  int destroy_jvm(lua_State *L) {
    try {
      JavaVM *vm = get_jvm(L);
      vm->DestroyJavaVM();
      lua_pushlightuserdata(L, (void *)&key);
      lua_pushnil(L);
      lua_settable(L, LUA_REGISTRYINDEX);
      return 0;
    } catch (std::exception &e) {
      lua_settop(L, 0);
      lua_pushnil(L);
      lua_pushstring(L, e.what());
      return 2;
    }
  }
}

inline namespace {
  inline namespace primitive {
#define SIGNATURE(name, signature) \
    struct java_##name { \
      static std::string java_sign() { \
        return #signature; \
      } \
    }
    SIGNATURE(boolean, Z);
    SIGNATURE(byte, B);
    SIGNATURE(char, C);
    SIGNATURE(short, S);
    SIGNATURE(int, I);
    SIGNATURE(long, J);
    SIGNATURE(float, F);
    SIGNATURE(double, D);
    SIGNATURE(void, V);
    SIGNATURE(object, Ljava/lang/Object;);
#undef SIGNATURE
  }

  template <typename T> struct method;
  template <typename R, typename... Args>
  struct method<R(Args...)> {
    static std::string java_sign() {
      return "(" + std::string{((Args::java_sign() + ... + ""))} + ")" + R::java_sign();
    }
  };
  namespace details {
    template <typename... Ts>
    struct get_arguments;
    template <>
    struct get_arguments<> {
      static std::vector<jvalue> lua2java(lua_State *, JNIEnv *) {
        return {};
      }
    };
    template <typename T, typename... Ts>
    struct get_arguments<T, Ts...> {
      static std::vector<jvalue> lua2java(lua_State *L, JNIEnv *env) {
        std::vector<jvalue> args = get_arguments<Ts...>::lua2java(L, env);
        jvalue arg;
        arg.l = T::lua2java(L, env);
        args.push_back(arg);
        return args;
      }
    };
  }
  template <typename... Ts>
  std::vector<jvalue> args_lua2java(lua_State *L, JNIEnv *env) {
    std::vector<jvalue> ret = details::get_arguments<Ts...>::lua2java(L, env);
    std::reverse(ret.begin(), ret.end());
    return ret;
  }
  template <typename>
  struct static_method;
  template <typename R, typename... Args>
  struct static_method<R(Args...)> {
    static int invoke(lua_State *L, JNIEnv *env, jclass cls, const char *name) {
      auto args = args_lua2java<Args...>(L, env);
      R::java2lua(L, env, call(env, cls, name, args.data()));
      return 1;
    }
    static jobject call(JNIEnv *env, jclass cls, const char *name, const jvalue *args) {
      jmethodID mid = env->GetStaticMethodID(cls, name, method<R(Args...)>::java_sign().c_str());
      if (mid == nullptr) throw std::runtime_error("method " + std::string{name} + " not found");
      jobject ret = env->CallStaticObjectMethodA(cls, mid, args);
      if (ret == nullptr) throw std::runtime_error("method " + std::string{name} + " returned null");
      return ret;
    }
  };
  template <typename... Args>
  struct static_method<java_void(Args...)> {
    static int invoke(lua_State *L, JNIEnv *env, jclass cls, const char *name) {
      jmethodID mid = env->GetStaticMethodID(cls, name, method<java_void(Args...)>::java_sign().c_str());
      if (mid == nullptr) throw std::runtime_error("method not found");
      auto args = args_lua2java<Args...>(L, env);
      env->CallStaticVoidMethodA(cls, mid, args.data());
      return 0;
    }
  };
  template <typename... Args>
  struct static_method<java_int(Args...)> {
    static int invoke(lua_State *L, JNIEnv *env, jclass cls, const char *name) {
      jmethodID mid = env->GetStaticMethodID(cls, name, method<java_int(Args...)>::java_sign().c_str());
      if (mid == nullptr) throw std::runtime_error("method not found");
      auto args = args_lua2java<Args...>(L, env);
      jint ret = env->CallStaticIntMethodA(cls, mid, args.data());
      if (env->ExceptionCheck()) throw std::runtime_error("method " + std::string{name} + " threw an exception");
      lua_pushinteger(L, ret);
      return 1;
    }
  };

  template <typename T>
  struct Class {
    static std::string java_sign() {
      return "L" + T::java_name() + ";";
    }
    static jclass java_class(JNIEnv *env) {
      jclass ret = env->FindClass(T::java_name().c_str());
      if (ret == nullptr) throw std::runtime_error("class " + T::java_name() + " not found");
      return ret;
    }
  };
  struct String: Class<String> {
    static std::string java_name() {
      return "java/lang/String";
    }
    static jobject lua2java(lua_State *L, JNIEnv *env) {
      if (!lua_isstring(L, -1)) throw std::runtime_error("expected string");
      size_t str_len = lua_objlen(L, -1);
      const char *str = lua_tostring(L, -1);
      std::vector<jchar> chars(str_len);
      for (size_t j = 0; j < str_len; ++j) {
        chars[j] = (unsigned char) str[j];
      }
      lua_pop(L, 1);
      jstring ret = env->NewString(chars.data(), str_len);
      if (ret == nullptr) throw std::runtime_error("string allocation failed");
      return ret;
    }
    static void java2lua(lua_State *L, JNIEnv *env, jobject obj) {
      jstring str = (jstring)obj;
      const jchar *chars = env->GetStringChars(str, NULL);
      if (chars == NULL) throw std::runtime_error("string allocation failed");
      size_t str_len = env->GetStringLength(str);
      std::vector<char> str_buf(str_len);
      for (size_t j = 0; j < str_len; ++j) {
        str_buf[j] = (char) chars[j];
      }
      env->ReleaseStringChars(str, chars);
      lua_pushlstring(L, str_buf.data(), str_len);
    }
  };
  struct Boolean: Class<Boolean> {
    static std::string java_name() {
      return "java/lang/Boolean";
    }
    static jobject lua2java(lua_State *L, JNIEnv *env) {
      if (!lua_isboolean(L, -1)) throw std::runtime_error("expected boolean");
      jvalue value;
      value.z = lua_toboolean(L, -1);
      lua_pop(L, 1);
      return static_method<Boolean(java_boolean)>::call(env, java_class(env), "valueOf", &value);
    }
    static void java2lua(lua_State *L, JNIEnv *env, jobject obj) {
      jboolean value = env->CallBooleanMethod(obj, env->GetMethodID(java_class(env), "booleanValue", method<java_boolean()>::java_sign().c_str()));
      if (env->ExceptionCheck()) throw std::runtime_error("exception thrown");
      lua_pushboolean(L, value);
    }
  };
  struct Integer: Class<Integer> {
    static std::string java_name() {
      return "java/lang/Integer";
    }
    static jobject lua2java(lua_State *L, JNIEnv *env) {
      if (!lua_isnumber(L, -1)) throw std::runtime_error("expected number");
      jvalue value;
      value.i = lua_tointeger(L, -1);
      lua_pop(L, 1);
      return static_method<Integer(java_int)>::call(env, java_class(env), "valueOf", &value);
    }
    static void java2lua(lua_State *L, JNIEnv *env, jobject obj) {
      jint value = env->CallIntMethod(obj, env->GetMethodID(java_class(env), "intValue", method<java_int()>::java_sign().c_str()));
      if (env->ExceptionCheck()) throw std::runtime_error("exception thrown");
      lua_pushinteger(L, value);
    }
  };

  template <typename T>
  struct array {
    static std::string java_sign() {
      return "[" + T::java_sign();
    }
    static jclass java_class(JNIEnv *env) {
      jclass ret = env->FindClass(java_sign().c_str());
      if (ret == nullptr) throw std::runtime_error("class " + java_sign() + " not found");
      return ret;
    }
    static jobject lua2java(lua_State *L, JNIEnv *env) {
      if (!lua_istable(L, -1)) throw std::runtime_error("expected table");
      size_t len = lua_objlen(L, -1);
      jobjectArray arr = env->NewObjectArray(len, T::java_class(env), NULL);
      if (arr == nullptr) throw std::runtime_error("array allocation failed");
      for (size_t i = 0; i < len; ++i) {
        lua_rawgeti(L, -1, i + 1);
        jobject obj = T::lua2java(L, env);
        env->SetObjectArrayElement(arr, i, obj);
        if (env->ExceptionCheck()) throw std::runtime_error("set array element failed");
        env->DeleteLocalRef(obj);
      }
      lua_pop(L, 1);
      return arr;
    }
    static void java2lua(lua_State *L, JNIEnv *env, jobject obj) {
      jobjectArray arr = (jobjectArray)obj;
      size_t len = env->GetArrayLength(arr);
      if (env->ExceptionCheck()) throw std::runtime_error("get array length failed");
      lua_createtable(L, len, 0);
      for (size_t i = 0; i < len; ++i) {
        jobject obj = env->GetObjectArrayElement(arr, i);
        if (obj == nullptr) throw std::runtime_error("array element is null");
        T::java2lua(L, env, obj);
        env->DeleteLocalRef(obj);
        lua_rawseti(L, -2, i + 1);
      }
    }
  };
  template <typename K, typename V>
  struct Pair: Class<Pair<K, V>> {
    static std::string java_name() {
      return "common/datastore/Pair";
    }
    static jobject lua2java(lua_State *L, JNIEnv *env) {
      if (!lua_istable(L, -1)) throw std::runtime_error("expected table(pair)");
      lua_rawgeti(L, -1, 1);
      jobject key = K::lua2java(L, env);
      lua_rawgeti(L, -1, 2);
      jobject value = V::lua2java(L, env);
      lua_pop(L, 1);
      jclass cls = Class<Pair<K, V>>::java_class(env);
      jmethodID mid = env->GetMethodID(cls, "<init>", method<java_void(java_object, java_object)>::java_sign().c_str());
      if (mid == nullptr) throw std::runtime_error("method Pair.<init> not found");
      jobject obj = env->NewObject(cls, mid, key, value);
      if (obj == nullptr) throw std::runtime_error("object allocation failed");
      env->DeleteLocalRef(key);
      env->DeleteLocalRef(value);
      return obj;
    }
    static void java2lua(lua_State *L, JNIEnv *env, jobject obj) {
      jclass cls = Class<Pair<K, V>>::java_class(env);
      jmethodID mid = env->GetMethodID(cls, "getKey", method<java_object()>::java_sign().c_str());
      if (mid == nullptr) throw std::runtime_error("method Pair.getKey not found");
      jobject key = env->CallObjectMethod(obj, mid);
      if (key == nullptr) throw std::runtime_error("key is null in Pair");
      mid = env->GetMethodID(cls, "getValue", method<java_object()>::java_sign().c_str());
      if (mid == nullptr) throw std::runtime_error("method Pair.getValue not found");
      jobject value = env->CallObjectMethod(obj, mid);
      if (value == nullptr) throw std::runtime_error("value is null in Pair");
      lua_createtable(L, 2, 0);
      K::java2lua(L, env, key);
      lua_rawseti(L, -2, 1);
      V::java2lua(L, env, value);
      lua_rawseti(L, -2, 2);
      env->DeleteLocalRef(key);
      env->DeleteLocalRef(value);
    }
  };
}

inline namespace {
  template <size_t N>
  struct string_literal {
    char data[N];
    constexpr string_literal(const char (&str)[N]) {
      for (size_t i = 0; i < N; ++i) data[i] = str[i];
    }
    constexpr operator const char*() const {
      return data;
    }
    constexpr operator std::string() const {
      return std::string(data, N - 1);
    }
  };
}

template <string_literal class_name, string_literal method_name, typename T>
struct call {
  static int f(lua_State *L) {
    try {
      auto_detach_env env = get_jvm_env(L);
      try {
        jclass cls = env->FindClass(class_name);
        if (cls == nullptr) throw std::runtime_error("class " + std::string(class_name) + " not found");
        return static_method<T>::invoke(L, env, cls, method_name);
      } catch (std::exception &e) {
        env->ExceptionClear();
        throw;
      }
    } catch (std::exception &e) {
      lua_settop(L, 0);
      lua_pushnil(L);
      lua_pushstring(L, e.what());
      return 2;
    }
  }
};

static luaL_Reg funcs[] = {
  {"start_jvm", start_jvm},
  // not sure whether it is invokable with only java.base
  // {"main", call<
  //     "entry/EntryPointMain",
  //     "main",
  //     java_int(array<String>)
  //   >::f},
  {"percent", call<
      "entry/percent/PercentEntryPoint",
      "run_invoked",
      array<Pair<array<String>, Boolean>>(Integer, Boolean, array<Boolean>, array<String>, Integer, String, String)
    >::f},
  {"destroy_jvm", destroy_jvm},
  {NULL, NULL}
};

extern "C" int luaopen_sfinder(lua_State *L) {
  luaL_openlib(L, "sfinder", funcs, 0);
  return 1;
}