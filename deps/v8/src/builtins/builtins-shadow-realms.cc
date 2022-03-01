// Copyright 2021 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/builtins/builtins-utils-inl.h"
#include "src/codegen/compiler.h"
#include "src/logging/counters.h"
#include "src/objects/js-shadow-realms-inl.h"

namespace v8 {
namespace internal {

// https://tc39.es/proposal-shadowrealm/#sec-shadowrealm-constructor
BUILTIN(ShadowRealmConstructor) {
  HandleScope scope(isolate);
  // 1. If NewTarget is undefined, throw a TypeError exception.
  if (args.new_target()->IsUndefined(isolate)) {  // [[Call]]
    THROW_NEW_ERROR_RETURN_FAILURE(
        isolate, NewTypeError(MessageTemplate::kConstructorNotFunction,
                              isolate->factory()->ShadowRealm_string()));
  }
  // [[Construct]]
  Handle<JSFunction> target = args.target();
  Handle<JSReceiver> new_target = Handle<JSReceiver>::cast(args.new_target());

  // 3. Let realmRec be CreateRealm().
  // 5. Let context be a new execution context.
  // 6. Set the Function of context to null.
  // 7. Set the Realm of context to realmRec.
  // 8. Set the ScriptOrModule of context to null.
  // 10. Perform ? SetRealmGlobalObject(realmRec, undefined, undefined).
  // 11. Perform ? SetDefaultGlobalBindings(O.[[ShadowRealm]]).
  // 12. Perform ? HostInitializeShadowRealm(O.[[ShadowRealm]]).
  // These steps are combined in
  // Isolate::RunHostCreateShadowRealmContextCallback and Context::New.
  // The host operation is hoisted for not creating a half-initialized
  // ShadowRealm object, which can fail the heap verification.
  Handle<NativeContext> native_context;
  ASSIGN_RETURN_FAILURE_ON_EXCEPTION(
      isolate, native_context,
      isolate->RunHostCreateShadowRealmContextCallback());

  // 2. Let O be ? OrdinaryCreateFromConstructor(NewTarget,
  // "%ShadowRealm.prototype%", « [[ShadowRealm]], [[ExecutionContext]] »).
  Handle<JSObject> result;
  ASSIGN_RETURN_FAILURE_ON_EXCEPTION(
      isolate, result,
      JSObject::New(target, new_target, Handle<AllocationSite>::null()));
  Handle<JSShadowRealm> O = Handle<JSShadowRealm>::cast(result);

  // 4. Set O.[[ShadowRealm]] to realmRec.
  // 9. Set O.[[ExecutionContext]] to context.
  O->set_native_context(*native_context);

  // 13. Return O.
  return *O;
}

namespace {

// https://tc39.es/proposal-shadowrealm/#sec-getwrappedvalue
MaybeHandle<Object> GetWrappedValue(Isolate* isolate, Handle<Object> value,
                                    Handle<NativeContext> creation_context,
                                    Handle<NativeContext> target_context) {
  // 1. If Type(value) is Object, then
  if (!value->IsJSReceiver()) {
    // 2. Return value.
    return value;
  }
  // 1a. If IsCallable(value) is false, throw a TypeError exception.
  if (!value->IsCallable()) {
    THROW_NEW_ERROR_RETURN_VALUE(
        isolate,
        NewError(Handle<JSFunction>(creation_context->type_error_function(),
                                    isolate),
                 MessageTemplate::kNotCallable),
        {});
  }
  // 1b. Return ? WrappedFunctionCreate(callerRealm, value).

  // WrappedFunctionCreate
  // https://tc39.es/proposal-shadowrealm/#sec-wrappedfunctioncreate

  // The intermediate wrapped functions are not user-visible. And calling a
  // wrapped function won't cause a side effect in the creation realm.
  // Unwrap here to avoid nested unwrapping at the call site.
  if (value->IsJSWrappedFunction()) {
    Handle<JSWrappedFunction> target_wrapped =
        Handle<JSWrappedFunction>::cast(value);
    value = Handle<Object>(target_wrapped->wrapped_target_function(), isolate);
  }

  // 1. Let internalSlotsList be the internal slots listed in Table 2, plus
  // [[Prototype]] and [[Extensible]].
  // 2. Let wrapped be ! MakeBasicObject(internalSlotsList).
  // 3. Set wrapped.[[Prototype]] to
  // callerRealm.[[Intrinsics]].[[%Function.prototype%]].
  // 4. Set wrapped.[[Call]] as described in 2.1.
  // 5. Set wrapped.[[WrappedTargetFunction]] to Target.
  // 6. Set wrapped.[[Realm]] to callerRealm.
  // 7. Let result be CopyNameAndLength(wrapped, Target, "wrapped").
  // 8. If result is an Abrupt Completion, throw a TypeError exception.
  Handle<JSWrappedFunction> wrapped =
      isolate->factory()->NewJSWrappedFunction(creation_context, value);

  // 9. Return wrapped.
  return wrapped;
}

}  // namespace

// https://tc39.es/proposal-shadowrealm/#sec-shadowrealm.prototype.evaluate
BUILTIN(ShadowRealmPrototypeEvaluate) {
  HandleScope scope(isolate);

  Handle<Object> source_text = args.atOrUndefined(isolate, 1);
  // 1. Let O be this value.
  Handle<Object> receiver = args.receiver();

  Factory* factory = isolate->factory();

  // 2. Perform ? ValidateShadowRealmObject(O).
  if (!receiver->IsJSShadowRealm()) {
    THROW_NEW_ERROR_RETURN_FAILURE(
        isolate, NewTypeError(MessageTemplate::kIncompatibleMethodReceiver));
  }
  Handle<JSShadowRealm> shadow_realm = Handle<JSShadowRealm>::cast(receiver);

  // 3. If Type(sourceText) is not String, throw a TypeError exception.
  if (!source_text->IsString()) {
    THROW_NEW_ERROR_RETURN_FAILURE(
        isolate,
        NewTypeError(MessageTemplate::kInvalidShadowRealmEvaluateSourceText));
  }

  // 4. Let callerRealm be the current Realm Record.
  Handle<NativeContext> caller_context = isolate->native_context();

  // 5. Let evalRealm be O.[[ShadowRealm]].
  Handle<NativeContext> eval_context =
      Handle<NativeContext>(shadow_realm->native_context(), isolate);
  // 6. Return ? PerformShadowRealmEval(sourceText, callerRealm, evalRealm).

  // PerformShadowRealmEval
  // https://tc39.es/proposal-shadowrealm/#sec-performshadowrealmeval
  // 1. Perform ? HostEnsureCanCompileStrings(callerRealm, evalRealm).
  // Run embedder pre-checks before executing the source code.
  MaybeHandle<String> validated_source;
  bool unhandled_object;
  std::tie(validated_source, unhandled_object) =
      Compiler::ValidateDynamicCompilationSource(isolate, eval_context,
                                                 source_text);
  if (unhandled_object) {
    THROW_NEW_ERROR_RETURN_FAILURE(
        isolate,
        NewTypeError(MessageTemplate::kInvalidShadowRealmEvaluateSourceText));
  }

  Handle<JSObject> eval_global_proxy(eval_context->global_proxy(), isolate);
  MaybeHandle<Object> result;
  bool is_parse_failed = false;
  {
    // 8. If runningContext is not already suspended, suspend runningContext.
    // 9. Let evalContext be a new ECMAScript code execution context.
    // 10. Set evalContext's Function to null.
    // 11. Set evalContext's Realm to evalRealm.
    // 12. Set evalContext's ScriptOrModule to null.
    // 13. Set evalContext's VariableEnvironment to varEnv.
    // 14. Set evalContext's LexicalEnvironment to lexEnv.
    // 15. Push evalContext onto the execution context stack; evalContext is now
    // the running execution context.
    SaveAndSwitchContext save(isolate, *eval_context);

    // 2. Perform the following substeps in an implementation-defined order,
    // possibly interleaving parsing and error detection:
    // 2a. Let script be ParseText(! StringToCodePoints(sourceText), Script).
    // 2b. If script is a List of errors, throw a SyntaxError exception.
    // 2c. If script Contains ScriptBody is false, return undefined.
    // 2d. Let body be the ScriptBody of script.
    // 2e. If body Contains NewTarget is true, throw a SyntaxError
    // exception.
    // 2f. If body Contains SuperProperty is true, throw a SyntaxError
    // exception.
    // 2g. If body Contains SuperCall is true, throw a SyntaxError exception.
    // 3. Let strictEval be IsStrict of script.
    // 4. Let runningContext be the running execution context.
    // 5. Let lexEnv be NewDeclarativeEnvironment(evalRealm.[[GlobalEnv]]).
    // 6. Let varEnv be evalRealm.[[GlobalEnv]].
    // 7. If strictEval is true, set varEnv to lexEnv.
    Handle<JSFunction> function;
    MaybeHandle<JSFunction> maybe_function =
        Compiler::GetFunctionFromValidatedString(eval_context, validated_source,
                                                 NO_PARSE_RESTRICTION,
                                                 kNoSourcePosition);
    if (maybe_function.is_null()) {
      is_parse_failed = true;
    } else {
      function = maybe_function.ToHandleChecked();

      // 16. Let result be EvalDeclarationInstantiation(body, varEnv,
      // lexEnv, null, strictEval).
      // 17. If result.[[Type]] is normal, then
      // 20a. Set result to the result of evaluating body.
      // 18. If result.[[Type]] is normal and result.[[Value]] is empty, then
      // 21a. Set result to NormalCompletion(undefined).
      result =
          Execution::Call(isolate, function, eval_global_proxy, 0, nullptr);

      // 19. Suspend evalContext and remove it from the execution context stack.
      // 20. Resume the context that is now on the top of the execution context
      // stack as the running execution context. Done by the scope.
    }
  }

  if (result.is_null()) {
    Handle<Object> pending_exception =
        Handle<Object>(isolate->pending_exception(), isolate);
    isolate->clear_pending_exception();
    if (is_parse_failed) {
      Handle<JSObject> error_object = Handle<JSObject>::cast(pending_exception);
      Handle<String> message = Handle<String>::cast(JSReceiver::GetDataProperty(
          isolate, error_object, factory->message_string()));

      return isolate->ReThrow(
          *factory->NewError(isolate->syntax_error_function(), message));
    }
    // 21. If result.[[Type]] is not normal, throw a TypeError exception.
    // TODO(v8:11989): provide a non-observable inspection.
    THROW_NEW_ERROR_RETURN_FAILURE(
        isolate, NewTypeError(MessageTemplate::kCallShadowRealmFunctionThrown));
  }
  // 22. Return ? GetWrappedValue(callerRealm, result.[[Value]]).
  Handle<Object> wrapped_result;
  ASSIGN_RETURN_FAILURE_ON_EXCEPTION(
      isolate, wrapped_result,
      GetWrappedValue(isolate, result.ToHandleChecked(), caller_context,
                      eval_context));
  return *wrapped_result;
}

// https://tc39.es/proposal-shadowrealm/#sec-shadowrealm.prototype.importvalue
BUILTIN(ShadowRealmPrototypeImportValue) {
  HandleScope scope(isolate);
  return ReadOnlyRoots(isolate).undefined_value();
}

}  // namespace internal
}  // namespace v8
