// Copyright (c) 2013 GitHub, Inc.
// Use of this source code is governed by the MIT license that can be
// found in the LICENSE file.

#include "NodeBindings.h"

#include <string>
#include <vector>

#include "mozilla/dom/ScriptSettings.h" // for AutoJSAPI
#include "base/message_loop.h"
#include "nsIFile.h"
#include "nsDirectoryService.h"
#include "nsDirectoryServiceDefs.h"
#include "env-inl.h"
#include "jsapi.h"
#include "nsString.h"
#include "nsAppRunner.h"
#include "nsContentUtils.h"
#include "nsJSPrincipals.h"

namespace mozilla {

NodeBindings::NodeBindings(bool is_browser)
    : is_browser_(is_browser),
      message_loop_(nullptr),
      uv_loop_(uv_default_loop()),
      embed_closed_(false),
      uv_env_(nullptr) {
}

NodeBindings::~NodeBindings() {
  // Quit the embed thread.
  embed_closed_ = true;
  uv_sem_post(&embed_sem_);
  WakeupEmbedThread();

  // Wait for everything to be done.
  uv_thread_join(&embed_thread_);

  // Clear uv.
  uv_sem_destroy(&embed_sem_);
  delete uv_env_;
  // delete context_scope;
  delete isolate_scope;
}

void NodeBindings::Initialize(JSContext* aContext, JSObject* aGlobal) {
  v8::V8::Initialize();
  uv_async_init(uv_default_loop(), &call_next_tick_async_, OnCallNextTick);
  call_next_tick_async_.data = this;

  v8::Isolate* isolate = v8::Isolate::New(aContext, aGlobal);
  // v8::Isolate::Scope isolate_scope(isolate);
  // TODO: FIX THIS LEAK
  isolate_scope = new v8::Isolate::Scope(isolate);
  v8::HandleScope handle_scope(isolate);
  // JSPrincipals* principals
  nsCOMPtr<nsIPrincipal> principal = nsContentUtils::GetSystemPrincipal();
  v8::Local<v8::Context> context = v8::Context::New(isolate, aGlobal, nsJSPrincipals::get(principal));
  v8::Context::Scope context_scope(context);
  // TODO: FIX THIS LEAK
  // v8::Context::Scope* context_scope = new v8::Context::Scope(context);

  dom::AutoEntryScript aes(aGlobal, "NodeBindings Initialize");

  node::Environment* env = CreateEnvironment(context);
  set_uv_env(env);
  uv_loop_ = uv_default_loop();
  PreMainMessageLoopRun();
  LoadEnvironment(env);
}

// In electron, this function is in atom bindings.
static void ActivateUVLoopCallback(const v8::FunctionCallbackInfo<v8::Value>& info) {
  v8::Local<v8::External> this_value = v8::Local<v8::External>::Cast(info.Data());
  NodeBindings* nodeBindings = static_cast<NodeBindings*>(this_value->Value());
  nodeBindings->ActivateUVLoop(v8::Isolate::GetCurrent());
}

node::Environment* NodeBindings::CreateEnvironment(
    v8::Handle<v8::Context> context) {
  // Build the path to the init script.
  nsCOMPtr<nsIFile> greDir;
  nsDirectoryService::gService->Get(NS_GRE_DIR,
                                    NS_GET_IID(nsIFile),
                                    getter_AddRefs(greDir));
  MOZ_ASSERT(greDir);
  greDir->AppendNative(NS_LITERAL_CSTRING("modules"));
  greDir->AppendNative(is_browser_ ? NS_LITERAL_CSTRING("browser") :
                                     NS_LITERAL_CSTRING("renderer"));
  greDir->AppendNative(NS_LITERAL_CSTRING("init.js"));
  nsAutoString initalScript;
  greDir->GetPath(initalScript);

  int exec_argc;
  const char** exec_argv;
  int argc = 2;
  char **argv = new char *[argc + 1];
  argv[0] = gArgv[0];
  argv[1] = ToNewCString(NS_LossyConvertUTF16toASCII(initalScript));
  node::Init(&argc, const_cast<const char**>(argv),  &exec_argc, &exec_argv);

  // Convert the app path to an absolute app path.
  nsCOMPtr<nsIFile> cwdDir;
  nsDirectoryService::gService->Get(NS_OS_CURRENT_WORKING_DIR,
                                    NS_GET_IID(nsIFile),
                                    getter_AddRefs(cwdDir));
  MOZ_ASSERT(cwdDir);

  char* absoluteAppPath = nullptr;
  nsDependentCString appPath(gArgv[1]);
  // Try the path as a relative path to the current working directory.
  nsresult rv = cwdDir->AppendRelativeNativePath(appPath);
  if (NS_SUCCEEDED(rv)) {
    nsString absoluteAppPathString;
    rv = cwdDir->GetPath(absoluteAppPathString);
    if (NS_FAILED(rv)) {
      MOZ_CRASH("Unable to build absolute app path.");
    }
    absoluteAppPath = ToNewCString(NS_LossyConvertUTF16toASCII(absoluteAppPathString));
  } else { // It's hopefully an absolute path.
    absoluteAppPath = gArgv[1];
  }

  v8::Isolate* isolate = context->GetIsolate();
  node::IsolateData* isolateData = node::CreateIsolateData(isolate, uv_default_loop());
  node::Environment* env = node::CreateEnvironment(
    isolateData,
    context,
    argc, argv, 0, nullptr);

  env->process_object()->Set(v8::String::NewFromUtf8(isolate, "resourcesPath"),
                             v8::String::NewFromUtf8(isolate, absoluteAppPath));
  env->process_object()->Set(v8::String::NewFromUtf8(isolate, "type"),
                             v8::String::NewFromUtf8(isolate, is_browser_ ? "browser" : "renderer"));

  v8::Local<v8::Value> this_value = v8::External::New(isolate, this);
  v8::Local<v8::FunctionTemplate> acc = v8::FunctionTemplate::New(isolate, ActivateUVLoopCallback, this_value);
  env->process_object()->Set(context, v8::String::NewFromUtf8(isolate,
                             "activateUvLoop"),
                             acc->GetFunction(context).ToLocalChecked());

  return env;
}

void NodeBindings::PreMainMessageLoopRun() {
  // Run user's main script before most things get initialized, so we can have
  // a chance to setup everything.
  PrepareMessageLoop();
  RunMessageLoop();
}

// In electron, this function is in atom bindings.
void NodeBindings::ActivateUVLoop(v8::Isolate* isolate) {
  // printf(">>> NodeBindings::ActivateUVLoop\n");
  node::Environment* env = node::Environment::GetCurrent(isolate);
  if (std::find(pending_next_ticks_.begin(), pending_next_ticks_.end(), env) !=
      pending_next_ticks_.end())
    return;

  pending_next_ticks_.push_back(env);
  uv_async_send(&call_next_tick_async_);
}

// In electron, this function is in atom bindings.
// static
void NodeBindings::OnCallNextTick(uv_async_t* handle) {
  NodeBindings* self = static_cast<NodeBindings*>(handle->data);
  for (std::list<node::Environment*>::const_iterator it =
           self->pending_next_ticks_.begin();
       it != self->pending_next_ticks_.end(); ++it) {
    node::Environment* env = *it;
    // KickNextTick, copied from node.cc:
    node::Environment::AsyncCallbackScope callback_scope(env);
    if (callback_scope.in_makecallback())
      continue;
    node::Environment::TickInfo* tick_info = env->tick_info();
    if (tick_info->length() == 0)
      env->isolate()->RunMicrotasks();
    v8::Local<v8::Object> process = env->process_object();
    if (tick_info->length() == 0)
      tick_info->set_index(0);
    env->tick_callback_function()->Call(process, 0, nullptr).IsEmpty();
  }

  self->pending_next_ticks_.clear();
}

void NodeBindings::LoadEnvironment(node::Environment* env) {
  node::LoadEnvironment(env);
  // TODO
  // mate::EmitEvent(env->isolate(), env->process_object(), "loaded");
}

void NodeBindings::PrepareMessageLoop() {
  MOZ_ASSERT(!is_browser_ || NS_IsMainThread());
  // Add dummy handle for libuv, otherwise libuv would quit when there is
  // nothing to do.
  uv_async_init(uv_loop_, &dummy_uv_handle_, nullptr);

  // Start worker that will interrupt main loop when having uv events.
  uv_sem_init(&embed_sem_, 0);
  uv_thread_create(&embed_thread_, EmbedThreadRunner, this);
}

void NodeBindings::RunMessageLoop() {
  MOZ_ASSERT(!is_browser_ || NS_IsMainThread());

  // The MessageLoop should have been created, remember the one in main thread.
  message_loop_ = MessageLoop::current();

  // Run uv loop for once to give the uv__io_poll a chance to add all events.
  UvRunOnce();
}

void NodeBindings::UvRunOnce() {
  printf(">>> NodeBindings::UvRunOnce\n");
  MOZ_ASSERT(!is_browser_ || NS_IsMainThread());
  node::Environment* env = uv_env();

  v8::Isolate::Scope isolate_scope(env->isolate());

  // TODO
  // Use Locker in browser process.
  // mate::Locker locker(env->isolate());
  v8::HandleScope handle_scope(env->isolate());

  // Enter node context while dealing with uv events.
  v8::Context::Scope context_scope(env->context());

  // Perform microtask checkpoint after running JavaScript.
  v8::MicrotasksScope script_scope(env->isolate(),
                                   v8::MicrotasksScope::kRunMicrotasks);

  JSContext* cx = JSContextFromIsolate(env->isolate());
  MOZ_ASSERT(cx);
  JSObject* globalObject = JS::CurrentGlobalOrNull(cx);
  MOZ_ASSERT(globalObject);
  dom::AutoEntryScript aes(globalObject, "NodeBindings UvRunOnce");

  // Deal with uv events.
  int r = uv_run(uv_loop_, UV_RUN_NOWAIT);
  if (r == 0) {
    // Quit from uv.
    // XXX: This hasn't been verified as the correct thing to do, but electron's
    // oringal code was message_loop_->QuitWhenIdle(), and this appears to be
    // the gecko equivalent.
    RefPtr<Runnable> task = new MessageLoop::QuitTask();
    message_loop_->PostIdleTask(task.forget());
  }

  // Tell the worker thread to continue polling.
  uv_sem_post(&embed_sem_);
}


void NodeBindings::WakeupMainThread() {
  MOZ_ASSERT(message_loop_);

  message_loop_->PostTask(NewRunnableMethod(this, &NodeBindings::UvRunOnce));
}

void NodeBindings::WakeupEmbedThread() {
  uv_async_send(&dummy_uv_handle_);
}

// static
void NodeBindings::EmbedThreadRunner(void *arg) {
  NodeBindings* self = static_cast<NodeBindings*>(arg);

  while (true) {
    // Wait for the main loop to deal with events.
    uv_sem_wait(&self->embed_sem_);
    if (self->embed_closed_)
      break;

    // Wait for something to happen in uv loop.
    // Note that the PollEvents() is implemented by derived classes, so when
    // this class is being destructed the PollEvents() would not be available
    // anymore. Because of it we must make sure we only invoke PollEvents()
    // when this class is alive.
    self->PollEvents();
    if (self->embed_closed_)
      break;

    // Deal with event in main thread.
    self->WakeupMainThread();
  }
}

}  // namespace atom
