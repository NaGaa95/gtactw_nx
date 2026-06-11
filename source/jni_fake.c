/* jni_fake.c -- fake JNI environment for the CTW 4.4.243 oswrapper layer
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <stdarg.h>
#include <string.h>

#include "config.h"
#include "util.h"
#include "jni_fake.h"
#include "movie_player.h"

#define JNI_OK 0
#define JNI_VERSION_1_6 0x00010006

typedef uint64_t juint;

// ---------------------------------------------------------------------------
// fake object model
// ---------------------------------------------------------------------------

enum {
  TAG_OBJECT = 0x4f424a31, // 'OBJ1'
  TAG_STRING = 0x53545231, // 'STR1'
  TAG_OBJARR = 0x4f415231, // 'OAR1'
  TAG_PRIARR = 0x50415231, // 'PAR1'
  TAG_ID     = 0x4d494431, // 'MID1'
};

typedef struct {
  uint32_t tag;
  char label[96];
} FakeObject;

typedef struct {
  uint32_t tag;
  char *utf;
} FakeString;

typedef struct {
  uint32_t tag;
  int len;
  void **items;
} FakeObjArray;

typedef struct {
  uint32_t tag;
  int len;
  int elem_size;
  void *data;
} FakePriArray;

// method and field IDs are pointers to these records; calls dispatch by name
typedef struct {
  uint32_t tag;
  char name[80];
  char sig[80];
} FakeID;

volatile int jni_quit_requested = 0;

void *jni_make_object(const char *label) {
  FakeObject *o = calloc(1, sizeof(*o));
  o->tag = TAG_OBJECT;
  strncpy(o->label, label, sizeof(o->label) - 1);
  return o;
}

void *jni_make_string(const char *utf) {
  FakeString *s = calloc(1, sizeof(*s));
  s->tag = TAG_STRING;
  s->utf = strdup(utf ? utf : "");
  return s;
}

void *jni_make_string_array(int n, const char **strs) {
  FakeObjArray *a = calloc(1, sizeof(*a));
  a->tag = TAG_OBJARR;
  a->len = n;
  a->items = calloc(n ? n : 1, sizeof(void *));
  for (int i = 0; i < n; i++)
    a->items[i] = jni_make_string(strs[i]);
  return a;
}

void *jni_make_int_array(int n, const int *vals) {
  FakePriArray *a = calloc(1, sizeof(*a));
  a->tag = TAG_PRIARR;
  a->len = n;
  a->elem_size = sizeof(int);
  a->data = calloc(n ? n : 1, sizeof(int));
  if (vals)
    memcpy(a->data, vals, n * sizeof(int));
  return a;
}

static const char *obj_str(void *jstr) {
  FakeString *s = jstr;
  if (s && s->tag == TAG_STRING)
    return s->utf;
  return "<not-a-string>";
}

// ---------------------------------------------------------------------------
// pending playlistOpen() completions (the game waits for the callback
// before it considers the playlist usable)
// ---------------------------------------------------------------------------

#define PLAYLIST_QUEUE_LEN 8
static JniPlaylistEvent playlist_queue[PLAYLIST_QUEUE_LEN];
static int playlist_head = 0, playlist_tail = 0;

static void push_playlist(int success, int count) {
  const int next = (playlist_tail + 1) % PLAYLIST_QUEUE_LEN;
  if (next != playlist_head) {
    playlist_queue[playlist_tail].success = success;
    playlist_queue[playlist_tail].count = count;
    playlist_tail = next;
  }
}

int jni_pop_pending_playlist(JniPlaylistEvent *event) {
  if (playlist_head == playlist_tail)
    return 0;
  if (event)
    *event = playlist_queue[playlist_head];
  playlist_head = (playlist_head + 1) % PLAYLIST_QUEUE_LEN;
  return 1;
}

// ---------------------------------------------------------------------------
// NvUtil app-local key/value store, persisted as tab-separated lines
// ---------------------------------------------------------------------------

#define KV_MAX 64

typedef struct {
  char key[64];
  char val[192];
} KvPair;

static KvPair kv_store[KV_MAX];
static int kv_count = 0;

static const char *kv_get(const char *key) {
  for (int i = 0; i < kv_count; i++)
    if (!strcmp(kv_store[i].key, key))
      return kv_store[i].val;
  return NULL;
}

static void kv_save(void) {
  FILE *f = fopen(APPSTATE_NAME, "w");
  if (!f)
    return;
  for (int i = 0; i < kv_count; i++)
    fprintf(f, "%s\t%s\n", kv_store[i].key, kv_store[i].val);
  fclose(f);
}

static void kv_set(const char *key, const char *val) {
  for (int i = 0; i < kv_count; i++) {
    if (!strcmp(kv_store[i].key, key)) {
      strlcpy(kv_store[i].val, val, sizeof(kv_store[i].val));
      kv_save();
      return;
    }
  }
  if (kv_count >= KV_MAX) {
    debugPrintf("JNI: app-local store full, dropping %s\n", key);
    return;
  }
  strlcpy(kv_store[kv_count].key, key, sizeof(kv_store[kv_count].key));
  strlcpy(kv_store[kv_count].val, val, sizeof(kv_store[kv_count].val));
  kv_count++;
  kv_save();
}

static void kv_load(void) {
  FILE *f = fopen(APPSTATE_NAME, "r");
  if (!f)
    return;
  char line[280];
  while (kv_count < KV_MAX && fgets(line, sizeof(line), f)) {
    char *tab = strchr(line, '\t');
    if (!tab)
      continue;
    *tab = 0;
    char *val = tab + 1;
    val[strcspn(val, "\r\n")] = 0;
    strlcpy(kv_store[kv_count].key, line, sizeof(kv_store[kv_count].key));
    strlcpy(kv_store[kv_count].val, val, sizeof(kv_store[kv_count].val));
    kv_count++;
  }
  fclose(f);
}

// ---------------------------------------------------------------------------
// pending Rockstar service callbacks
// ---------------------------------------------------------------------------

#define ROCKSTAR_QUEUE_LEN 32
static JniRockstarEvent rockstar_queue[ROCKSTAR_QUEUE_LEN];
static int rockstar_head = 0, rockstar_tail = 0;

static void push_rockstar(JniRockstarEvent event) {
  const int next = (rockstar_tail + 1) % ROCKSTAR_QUEUE_LEN;
  if (next == rockstar_head) {
    debugPrintf("JNI: dropped rockstar event %d (queue full)\n", event.type);
    return;
  }
  rockstar_queue[rockstar_tail] = event;
  rockstar_tail = next;
}

static void push_rockstar_type(JniRockstarEventType type) {
  JniRockstarEvent event;
  memset(&event, 0, sizeof(event));
  event.type = type;
  push_rockstar(event);
}

int jni_pop_pending_rockstar(JniRockstarEvent *event) {
  if (rockstar_head == rockstar_tail)
    return 0;
  if (event)
    *event = rockstar_queue[rockstar_head];
  rockstar_head = (rockstar_head + 1) % ROCKSTAR_QUEUE_LEN;
  return 1;
}

// ---------------------------------------------------------------------------
// method/field ID pool
// ---------------------------------------------------------------------------

#define MAX_IDS 256
static FakeID id_pool[MAX_IDS];
static int id_count = 0;

static FakeID *get_id(const char *name, const char *sig) {
  for (int i = 0; i < id_count; i++) {
    if (!strcmp(id_pool[i].name, name) && !strcmp(id_pool[i].sig, sig))
      return &id_pool[i];
  }
  if (id_count >= MAX_IDS) {
    debugPrintf("JNI: id pool exhausted!\n");
    return &id_pool[0];
  }
  FakeID *id = &id_pool[id_count++];
  id->tag = TAG_ID;
  strncpy(id->name, name, sizeof(id->name) - 1);
  strncpy(id->sig, sig, sizeof(id->sig) - 1);
  return id;
}

// ---------------------------------------------------------------------------
// GamePlatformServices method dispatch
// (signatures listed in PORT_NOTES_2.1.131.md)
// ---------------------------------------------------------------------------

static juint call_boolean(const char *name, va_list va) {
  if (!strcmp(name, "isMoviePlaying"))
    return movie_is_playing();
  if (!strcmp(name, "isSplashVisible")) {
    // the splash screen lives on the Java side; we never show one
    return 0;
  }
  if (!strcmp(name, "hasAppLocalValue")) {
    const char *key = obj_str(va_arg(va, void *));
    const int has = kv_get(key) != NULL;
    debugPrintf("JNI: hasAppLocalValue(%s) -> %d\n", key, has);
    return has;
  }
  // playlistIsPlaying, rockstarInTrial -> false for now
  debugPrintf("JNI: CallBooleanMethod(%s) -> false\n", name);
  return 0;
}

static juint call_int(const char *name, va_list va) {
  (void)va;
  if (!strcmp(name, "getDeviceLocale")) {
    // TODO: map the system language to whatever enum the Java side returns;
    // exposed through config.language for now
    debugPrintf("JNI: getDeviceLocale -> %d\n", config.language);
    return config.language;
  }
  if (!strcmp(name, "playlistCount"))
    return 0; // TODO: native playlist player
  debugPrintf("JNI: CallIntMethod(%s) -> 0\n", name);
  return 0;
}

static void *call_object(const char *name, va_list va) {
  if (!strcmp(name, "getAppLocalValue")) {
    const char *key = obj_str(va_arg(va, void *));
    const char *val = kv_get(key);
    debugPrintf("JNI: getAppLocalValue(%s) -> %s\n", key, val ? val : "(null)");
    return val ? jni_make_string(val) : NULL;
  }
  if (!strcmp(name, "getParameter")) {
    // NvUtil launch parameters; we have none
    const char *key = obj_str(va_arg(va, void *));
    debugPrintf("JNI: getParameter(%s) -> null\n", key);
    return NULL;
  }
  debugPrintf("JNI: CallObjectMethod(%s) -> null\n", name);
  return NULL;
}

static void call_void(const char *name, va_list va) {
  if (!strcmp(name, "playMovie")) {
    void *path = va_arg(va, void *);
    const int skippable = va_arg(va, int);
    debugPrintf("JNI: playMovie(%s, skippable=%d)\n", obj_str(path), skippable);
    movie_play(obj_str(path), skippable);
    return;
  }
  if (!strcmp(name, "stopMovie")) {
    debugPrintf("JNI: stopMovie\n");
    movie_stop();
    return;
  }
  if (!strcmp(name, "setMovieText")) {
    void *str = va_arg(va, void *);
    const char *text = str ? obj_str(str) : "";
    debugPrintf("JNI: setMovieText(%s)\n", text);
    movie_set_text(text);
    return;
  }
  if (!strcmp(name, "setMovieTextScale")) {
    movie_set_text_scale(va_arg(va, int));
    return;
  }
  if (!strcmp(name, "setAppLocalValue")) {
    void *key = va_arg(va, void *);
    void *val = va_arg(va, void *);
    debugPrintf("JNI: setAppLocalValue(%s, %s)\n", obj_str(key), obj_str(val));
    kv_set(obj_str(key), obj_str(val));
    return;
  }
  if (!strcmp(name, "playlistOpen")) {
    // TODO: native mpg123+OpenAL playlist player for the radio; report
    // failure for now so the game doesn't wait for the callback forever
    void *str = va_arg(va, void *);
    debugPrintf("JNI: playlistOpen(%s) -> queue failure\n", obj_str(str));
    push_playlist(0, 0);
    return;
  }
  if (!strcmp(name, "playlistPlay") || !strcmp(name, "playlistStop") ||
      !strcmp(name, "playlistPause")) {
    debugPrintf("JNI: %s ignored\n", name);
    return;
  }
  if (!strcmp(name, "rockstarShowInitial")) {
    debugPrintf("JNI: rockstarShowInitial -> queue complete\n");
    push_rockstar_type(JNI_RS_INITIAL_COMPLETE);
    return;
  }
  if (!strcmp(name, "rockstarShowGate")) {
    JniRockstarEvent event;
    memset(&event, 0, sizeof(event));
    event.type = JNI_RS_GATE_COMPLETE;
    event.accepted = 1;
    debugPrintf("JNI: rockstarShowGate -> queue complete accepted=1\n");
    push_rockstar(event);
    return;
  }
  if (!strcmp(name, "rockstarSignIn")) {
    JniRockstarEvent event;
    memset(&event, 0, sizeof(event));
    event.type = JNI_RS_STATE_CHANGED;
    event.signed_in = 0;
    debugPrintf("JNI: rockstarSignIn -> queue offline complete\n");
    push_rockstar(event);
    push_rockstar_type(JNI_RS_SIGN_IN_COMPLETE);
    return;
  }
  if (!strcmp(name, "rockstarSignOut")) {
    JniRockstarEvent event;
    memset(&event, 0, sizeof(event));
    event.type = JNI_RS_STATE_CHANGED;
    event.signed_in = 0;
    debugPrintf("JNI: rockstarSignOut -> queue complete\n");
    push_rockstar(event);
    push_rockstar_type(JNI_RS_SIGN_OUT_COMPLETE);
    return;
  }
  if (!strcmp(name, "rockstarShowCloudDisabled")) {
    debugPrintf("JNI: rockstarShowCloudDisabled -> queue complete\n");
    push_rockstar_type(JNI_RS_CLOUD_DISABLED_COMPLETE);
    return;
  }
  if (!strcmp(name, "rockstarFetchId")) {
    JniRockstarEvent event;
    memset(&event, 0, sizeof(event));
    event.type = JNI_RS_ID_CHANGED;
    event.value = NULL; // dispatched as an empty string
    debugPrintf("JNI: rockstarFetchId -> queue empty id\n");
    push_rockstar(event);
    return;
  }
  if (!strcmp(name, "rockstarSetLocalePriority")) {
    void *str = va_arg(va, void *);
    debugPrintf("JNI: rockstarSetLocalePriority(%s) ignored\n", obj_str(str));
    return;
  }
  // rockstarRequestReview -> no-op
  debugPrintf("JNI: CallVoidMethod(%s) ignored\n", name);
}

// ---------------------------------------------------------------------------
// DeviceInfo field dispatch
// ---------------------------------------------------------------------------

static juint get_boolean_field(const char *name) {
  juint v = 0;
  if (!strcmp(name, "isTvDevice"))    v = 1; // controller-first UI
  if (!strcmp(name, "isPhone"))       v = 0;
  if (!strcmp(name, "hasTouchScreen"))v = config.touchscreen ? 1 : 0; // panel events fed from main.c
  if (!strcmp(name, "hasVibrator"))   v = 0;
  debugPrintf("JNI: GetBooleanField(%s) -> %d\n", name, (int)v);
  return v;
}

static juint get_int_field(const char *name) {
  juint v = 0;
  if (!strcmp(name, "osVersion"))    v = 30;
  if (!strcmp(name, "cpuFrequency")) v = 1785;
  debugPrintf("JNI: GetIntField(%s) -> %d\n", name, (int)v);
  return v;
}

static void *get_object_field(const char *name) {
  if (!strcmp(name, "manufacturer")) return jni_make_string("Nintendo");
  if (!strcmp(name, "model"))        return jni_make_string("Switch");
  if (!strcmp(name, "hardware"))     return jni_make_string("nx");
  if (!strcmp(name, "product"))      return jni_make_string("switch");
  debugPrintf("JNI: GetObjectField(%s) -> null\n", name);
  return NULL;
}

// ---------------------------------------------------------------------------
// JNIEnv function table
// ---------------------------------------------------------------------------

static juint j_GetVersion(void *env) {
  (void)env;
  return JNI_VERSION_1_6;
}

static void *j_FindClass(void *env, const char *name) {
  (void)env;
  debugPrintf("JNI: FindClass(%s)\n", name);
  return jni_make_object(name);
}

static void *j_GetMethodID(void *env, void *cls, const char *name, const char *sig) {
  (void)env; (void)cls;
  debugPrintf("JNI: GetMethodID(%s %s)\n", name, sig);
  return get_id(name, sig);
}

static void *j_GetFieldID(void *env, void *cls, const char *name, const char *sig) {
  (void)env; (void)cls;
  debugPrintf("JNI: GetFieldID(%s %s)\n", name, sig);
  return get_id(name, sig);
}

static void *j_GetObjectClass(void *env, void *obj) {
  (void)env; (void)obj;
  return jni_make_object("class");
}

static void *j_NewGlobalRef(void *env, void *obj) { (void)env; return obj; }
static void *j_NewLocalRef(void *env, void *obj) { (void)env; return obj; }
static juint j_ret0_2(void *env, void *a) { (void)env; (void)a; return 0; }
static juint j_ret0_3(void *env, void *a, void *b) { (void)env; (void)a; (void)b; return 0; }

// --- Call<type>Method -------------------------------------------------------

static juint j_CallBooleanMethodV(void *env, void *obj, FakeID *id, va_list va) {
  (void)env; (void)obj;
  return call_boolean(id->name, va);
}
static juint j_CallBooleanMethod(void *env, void *obj, FakeID *id, ...) {
  va_list va;
  va_start(va, id);
  juint r = call_boolean(id->name, va);
  va_end(va);
  return r;
}

static juint j_CallIntMethodV(void *env, void *obj, FakeID *id, va_list va) {
  (void)env; (void)obj;
  return call_int(id->name, va);
}
static juint j_CallIntMethod(void *env, void *obj, FakeID *id, ...) {
  va_list va;
  va_start(va, id);
  juint r = call_int(id->name, va);
  va_end(va);
  return r;
}

static void *j_CallObjectMethodV(void *env, void *obj, FakeID *id, va_list va) {
  (void)env; (void)obj;
  return call_object(id->name, va);
}
static void *j_CallObjectMethod(void *env, void *obj, FakeID *id, ...) {
  va_list va;
  va_start(va, id);
  void *r = call_object(id->name, va);
  va_end(va);
  return r;
}

static void j_CallVoidMethodV(void *env, void *obj, FakeID *id, va_list va) {
  (void)env; (void)obj;
  call_void(id->name, va);
}
static void j_CallVoidMethod(void *env, void *obj, FakeID *id, ...) {
  va_list va;
  va_start(va, id);
  call_void(id->name, va);
  va_end(va);
}

static juint j_CallLongMethodV(void *env, void *obj, FakeID *id, va_list va) {
  (void)env; (void)obj; (void)va;
  debugPrintf("JNI: CallLongMethod(%s) -> 0\n", id->name);
  return 0;
}

static float j_CallFloatMethodV(void *env, void *obj, FakeID *id, va_list va) {
  (void)env; (void)obj; (void)va;
  debugPrintf("JNI: CallFloatMethod(%s) -> 0\n", id->name);
  return 0.0f;
}

// static variants share the dispatchers (we don't distinguish receivers)
static void *j_CallStaticObjectMethodV(void *env, void *cls, FakeID *id, va_list va) {
  (void)env; (void)cls;
  return call_object(id->name, va);
}
static void *j_CallStaticObjectMethod(void *env, void *cls, FakeID *id, ...) {
  va_list va;
  va_start(va, id);
  void *r = call_object(id->name, va);
  va_end(va);
  return r;
}
static void j_CallStaticVoidMethodV(void *env, void *cls, FakeID *id, va_list va) {
  (void)env; (void)cls;
  call_void(id->name, va);
}
static void j_CallStaticVoidMethod(void *env, void *cls, FakeID *id, ...) {
  va_list va;
  va_start(va, id);
  call_void(id->name, va);
  va_end(va);
}

static void *j_NewObjectV(void *env, void *cls, FakeID *id, va_list va) {
  (void)env; (void)va;
  FakeObject *c = cls;
  debugPrintf("JNI: NewObject(%s)\n", (c && c->tag == TAG_OBJECT) ? c->label : "?");
  return jni_make_object("newobject");
}
static void *j_NewObject(void *env, void *cls, FakeID *id, ...) {
  va_list va;
  va_start(va, id);
  void *r = j_NewObjectV(env, cls, id, va);
  va_end(va);
  return r;
}

// --- fields ------------------------------------------------------------------

static void *j_GetObjectField(void *env, void *obj, FakeID *id) {
  (void)env; (void)obj;
  return get_object_field(id->name);
}
static juint j_GetBooleanField(void *env, void *obj, FakeID *id) {
  (void)env; (void)obj;
  return get_boolean_field(id->name);
}
static juint j_GetIntField(void *env, void *obj, FakeID *id) {
  (void)env; (void)obj;
  return get_int_field(id->name);
}
static juint j_GetLongField(void *env, void *obj, FakeID *id) {
  (void)env; (void)obj;
  debugPrintf("JNI: GetLongField(%s) -> 0\n", id->name);
  return 0;
}
static float j_GetFloatField(void *env, void *obj, FakeID *id) {
  (void)env; (void)obj;
  debugPrintf("JNI: GetFloatField(%s) -> 0\n", id->name);
  return 0.0f;
}

// --- strings ------------------------------------------------------------------

static void *j_NewStringUTF(void *env, const char *utf) {
  (void)env;
  return jni_make_string(utf);
}

static const char *j_GetStringUTFChars(void *env, void *jstr, uint8_t *is_copy) {
  (void)env;
  if (is_copy) *is_copy = 0;
  return obj_str(jstr);
}

static void j_ReleaseStringUTFChars(void *env, void *jstr, const char *utf) {
  (void)env; (void)jstr; (void)utf;
}

static juint j_GetStringUTFLength(void *env, void *jstr) {
  (void)env;
  return strlen(obj_str(jstr));
}

static juint j_GetStringLength(void *env, void *jstr) {
  (void)env;
  return strlen(obj_str(jstr));
}

static const uint16_t *j_GetStringChars(void *env, void *jstr, uint8_t *is_copy) {
  (void)env;
  if (is_copy) *is_copy = 1;
  const char *utf = obj_str(jstr);
  const int len = strlen(utf);
  uint16_t *out = calloc(len + 1, sizeof(uint16_t));
  for (int i = 0; i < len; i++)
    out[i] = (uint8_t)utf[i];
  return out;
}

static void j_ReleaseStringChars(void *env, void *jstr, const uint16_t *chars) {
  (void)env; (void)jstr;
  free((void *)chars);
}

// --- arrays -------------------------------------------------------------------

static juint j_GetArrayLength(void *env, void *arr) {
  (void)env;
  FakeObjArray *a = arr;
  if (a && (a->tag == TAG_OBJARR || a->tag == TAG_PRIARR))
    return a->len;
  debugPrintf("JNI: GetArrayLength on non-array %p\n", arr);
  return 0;
}

static void *j_GetObjectArrayElement(void *env, void *arr, int idx) {
  (void)env;
  FakeObjArray *a = arr;
  if (a && a->tag == TAG_OBJARR && idx >= 0 && idx < a->len)
    return a->items[idx];
  debugPrintf("JNI: GetObjectArrayElement OOB %p[%d] len=%d -> empty string\n",
      arr, idx, (a && a->tag == TAG_OBJARR) ? a->len : -1);
  return jni_make_string("");
}

static void j_SetObjectArrayElement(void *env, void *arr, int idx, void *val) {
  (void)env;
  FakeObjArray *a = arr;
  if (a && a->tag == TAG_OBJARR && idx >= 0 && idx < a->len)
    a->items[idx] = val;
}

static void *j_NewObjectArray(void *env, int len, void *cls, void *init) {
  (void)env; (void)cls;
  FakeObjArray *a = calloc(1, sizeof(*a));
  a->tag = TAG_OBJARR;
  a->len = len;
  a->items = calloc(len ? len : 1, sizeof(void *));
  for (int i = 0; i < len; i++)
    a->items[i] = init;
  return a;
}

static void *new_pri_array(int len, int elem_size) {
  FakePriArray *a = calloc(1, sizeof(*a));
  a->tag = TAG_PRIARR;
  a->len = len;
  a->elem_size = elem_size;
  a->data = calloc(len ? len : 1, elem_size);
  return a;
}

static void *j_NewByteArray(void *env, int len)   { (void)env; return new_pri_array(len, 1); }
static void *j_NewIntArray(void *env, int len)    { (void)env; return new_pri_array(len, 4); }
static void *j_NewFloatArray(void *env, int len)  { (void)env; return new_pri_array(len, 4); }

static void *j_GetPriArrayElements(void *env, void *arr, uint8_t *is_copy) {
  (void)env;
  if (is_copy) *is_copy = 0;
  FakePriArray *a = arr;
  if (a && a->tag == TAG_PRIARR)
    return a->data;
  return NULL;
}

static void j_ReleasePriArrayElements(void *env, void *arr, void *elems, int mode) {
  (void)env; (void)arr; (void)elems; (void)mode;
}

static void j_GetPriArrayRegion(void *env, void *arr, int start, int len, void *buf) {
  (void)env;
  FakePriArray *a = arr;
  if (a && a->tag == TAG_PRIARR && start >= 0 && start + len <= a->len)
    memcpy(buf, (char *)a->data + start * a->elem_size, len * a->elem_size);
}

static void j_SetPriArrayRegion(void *env, void *arr, int start, int len, const void *buf) {
  (void)env;
  FakePriArray *a = arr;
  if (a && a->tag == TAG_PRIARR && start >= 0 && start + len <= a->len)
    memcpy((char *)a->data + start * a->elem_size, buf, len * a->elem_size);
}

// --- misc ----------------------------------------------------------------------

static juint j_RegisterNatives(void *env, void *cls, void *methods, int n) {
  (void)env; (void)cls; (void)methods;
  debugPrintf("JNI: RegisterNatives(%d methods) ignored\n", n);
  return 0;
}

static juint j_GetJavaVM(void *env, void **vm) {
  (void)env;
  *vm = fake_vm;
  return JNI_OK;
}

static juint j_ExceptionCheck(void *env) { (void)env; return 0; }
static void *j_ExceptionOccurred(void *env) { (void)env; return NULL; }
static void j_ExceptionClearDescribe(void *env) { (void)env; }
static void j_DeleteRef(void *env, void *obj) { (void)env; (void)obj; }
static juint j_PushLocalFrame(void *env, int cap) { (void)env; (void)cap; return 0; }
static void *j_PopLocalFrame(void *env, void *result) { (void)env; return result; }

// generic catch-all
static juint j_unimplemented(void) {
  debugPrintf("JNI: call to unimplemented function slot\n");
  return 0;
}

// ---------------------------------------------------------------------------
// table assembly (indices per the JNI specification)
// ---------------------------------------------------------------------------

static void *env_table[233];
static void **env_table_ptr = env_table;
void *fake_env = &env_table_ptr;

// JavaVM
static juint vm_DestroyJavaVM(void *vm) { (void)vm; return JNI_OK; }
static juint vm_AttachCurrentThread(void *vm, void **env, void *args) {
  (void)vm; (void)args;
  if (env) *env = fake_env;
  return JNI_OK;
}
static juint vm_DetachCurrentThread(void *vm) { (void)vm; return JNI_OK; }
static juint vm_GetEnv(void *vm, void **env, int version) {
  (void)vm; (void)version;
  if (env) *env = fake_env;
  return JNI_OK;
}

static void *vm_table[8];
static void **vm_table_ptr = vm_table;
void *fake_vm = &vm_table_ptr;

void jni_init(void) {
  kv_load();

  // the game concatenates the storage root with file names (Adjustable.cfg,
  // saves); an unset root would put those at the SD card root, so anchor
  // both roots to the game directory
  if (!kv_get("STORAGE_ROOT"))
    kv_set("STORAGE_ROOT", ".");
  if (!kv_get("STORAGE_ROOT_BASE"))
    kv_set("STORAGE_ROOT_BASE", ".");

  for (int i = 0; i < 233; i++)
    env_table[i] = (void *)j_unimplemented;

  env_table[4]  = (void *)j_GetVersion;
  env_table[6]  = (void *)j_FindClass;
  env_table[15] = (void *)j_ExceptionOccurred;
  env_table[16] = (void *)j_ExceptionClearDescribe; // ExceptionDescribe
  env_table[17] = (void *)j_ExceptionClearDescribe; // ExceptionClear
  env_table[19] = (void *)j_PushLocalFrame;
  env_table[20] = (void *)j_PopLocalFrame;
  env_table[21] = (void *)j_NewGlobalRef;
  env_table[22] = (void *)j_DeleteRef;  // DeleteGlobalRef
  env_table[23] = (void *)j_DeleteRef;  // DeleteLocalRef
  env_table[24] = (void *)j_ret0_3;     // IsSameObject
  env_table[25] = (void *)j_NewLocalRef;
  env_table[26] = (void *)j_ret0_2;     // EnsureLocalCapacity
  env_table[28] = (void *)j_NewObject;
  env_table[29] = (void *)j_NewObjectV;
  env_table[31] = (void *)j_GetObjectClass;
  env_table[33] = (void *)j_GetMethodID;
  env_table[34] = (void *)j_CallObjectMethod;
  env_table[35] = (void *)j_CallObjectMethodV;
  env_table[37] = (void *)j_CallBooleanMethod;
  env_table[38] = (void *)j_CallBooleanMethodV;
  env_table[49] = (void *)j_CallIntMethod;
  env_table[50] = (void *)j_CallIntMethodV;
  env_table[53] = (void *)j_CallLongMethodV;
  env_table[56] = (void *)j_CallFloatMethodV;
  env_table[61] = (void *)j_CallVoidMethod;
  env_table[62] = (void *)j_CallVoidMethodV;
  env_table[94] = (void *)j_GetFieldID;
  env_table[95] = (void *)j_GetObjectField;
  env_table[96] = (void *)j_GetBooleanField;
  env_table[100] = (void *)j_GetIntField;
  env_table[101] = (void *)j_GetLongField;
  env_table[102] = (void *)j_GetFloatField;
  env_table[113] = (void *)j_GetMethodID; // GetStaticMethodID
  env_table[114] = (void *)j_CallStaticObjectMethod;
  env_table[115] = (void *)j_CallStaticObjectMethodV;
  env_table[141] = (void *)j_CallStaticVoidMethod;
  env_table[142] = (void *)j_CallStaticVoidMethodV;
  env_table[144] = (void *)j_GetFieldID; // GetStaticFieldID
  env_table[164] = (void *)j_GetStringLength;
  env_table[165] = (void *)j_GetStringChars;
  env_table[166] = (void *)j_ReleaseStringChars;
  env_table[167] = (void *)j_NewStringUTF;
  env_table[168] = (void *)j_GetStringUTFLength;
  env_table[169] = (void *)j_GetStringUTFChars;
  env_table[170] = (void *)j_ReleaseStringUTFChars;
  env_table[171] = (void *)j_GetArrayLength;
  env_table[172] = (void *)j_NewObjectArray;
  env_table[173] = (void *)j_GetObjectArrayElement;
  env_table[174] = (void *)j_SetObjectArrayElement;
  env_table[176] = (void *)j_NewByteArray;
  env_table[179] = (void *)j_NewIntArray;
  env_table[181] = (void *)j_NewFloatArray;
  env_table[183] = (void *)j_GetPriArrayElements; // boolean
  env_table[184] = (void *)j_GetPriArrayElements; // byte
  env_table[185] = (void *)j_GetPriArrayElements; // char
  env_table[186] = (void *)j_GetPriArrayElements; // short
  env_table[187] = (void *)j_GetPriArrayElements; // int
  env_table[188] = (void *)j_GetPriArrayElements; // long
  env_table[189] = (void *)j_GetPriArrayElements; // float
  env_table[190] = (void *)j_GetPriArrayElements; // double
  for (int i = 191; i <= 198; i++)
    env_table[i] = (void *)j_ReleasePriArrayElements;
  for (int i = 199; i <= 206; i++)
    env_table[i] = (void *)j_GetPriArrayRegion;
  for (int i = 207; i <= 214; i++)
    env_table[i] = (void *)j_SetPriArrayRegion;
  env_table[215] = (void *)j_RegisterNatives;
  env_table[216] = (void *)j_ret0_2;    // UnregisterNatives
  env_table[217] = (void *)j_ret0_2;    // MonitorEnter
  env_table[218] = (void *)j_ret0_2;    // MonitorExit
  env_table[219] = (void *)j_GetJavaVM;
  env_table[222] = (void *)j_GetPriArrayElements;     // GetPrimitiveArrayCritical
  env_table[223] = (void *)j_ReleasePriArrayElements; // ReleasePrimitiveArrayCritical
  env_table[226] = (void *)j_NewGlobalRef; // NewWeakGlobalRef
  env_table[227] = (void *)j_DeleteRef;    // DeleteWeakGlobalRef
  env_table[228] = (void *)j_ExceptionCheck;

  vm_table[3] = (void *)vm_DestroyJavaVM;
  vm_table[4] = (void *)vm_AttachCurrentThread;
  vm_table[5] = (void *)vm_DetachCurrentThread;
  vm_table[6] = (void *)vm_GetEnv;
  vm_table[7] = (void *)vm_AttachCurrentThread; // AttachCurrentThreadAsDaemon

  debugPrintf("JNI: fake environment initialized (env=%p vm=%p)\n", fake_env, fake_vm);
}
