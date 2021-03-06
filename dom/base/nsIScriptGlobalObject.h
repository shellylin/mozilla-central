/* -*- Mode: C++; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=2 sw=2 et tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef nsIScriptGlobalObject_h__
#define nsIScriptGlobalObject_h__

#include "nsISupports.h"
#include "nsIGlobalObject.h"
#include "js/TypeDecls.h"
#include "mozilla/EventForwards.h"

class nsIScriptContext;
class nsIScriptGlobalObject;

// A helper function for nsIScriptGlobalObject implementations to use
// when handling a script error.  Generally called by the global when a context
// notifies it of an error via nsIScriptGlobalObject::HandleScriptError.
// Returns true if HandleDOMEvent was actually called, in which case
// aStatus will be filled in with the status.
bool
NS_HandleScriptError(nsIScriptGlobalObject *aScriptGlobal,
                     mozilla::InternalScriptErrorEvent *aErrorEvent,
                     nsEventStatus *aStatus);


#define NS_ISCRIPTGLOBALOBJECT_IID \
{ 0xa6c0bfae, 0x8be4, 0x4747, \
  { 0xaf, 0x1a, 0xe3, 0xf0, 0x3f, 0xb6, 0x0e, 0xb8 } }

/**
 * The global object which keeps a script context for each supported script
 * language. This often used to store per-window global state.
 * This is a heavyweight interface implemented only by DOM globals, and
 * it might go away some time in the future.
 */

class nsIScriptGlobalObject : public nsIGlobalObject
{
public:
  NS_DECLARE_STATIC_IID_ACCESSOR(NS_ISCRIPTGLOBALOBJECT_IID)

  /**
   * Ensure that the script global object is initialized for working with the
   * specified script language ID.  This will set up the nsIScriptContext
   * and 'script global' for that language, allowing these to be fetched
   * and manipulated.
   * @return NS_OK if successful; error conditions include that the language
   * has not been registered, as well as 'normal' errors, such as
   * out-of-memory
   */
  virtual nsresult EnsureScriptEnvironment() = 0;
  /**
   * Get a script context (WITHOUT added reference) for the specified language.
   */
  virtual nsIScriptContext *GetScriptContext() = 0;

  nsIScriptContext* GetContext() {
    return GetScriptContext();
  }

  /**
   * Called when the global script for a language is finalized, typically as
   * part of its GC process.  By the time this call is made, the
   * nsIScriptContext for the language has probably already been removed.
   * After this call, the passed object is dead - which should generally be the
   * same object the global is using for a global for that language.
   */
  virtual void OnFinalize(JSObject* aObject) = 0;

  /**
   * Handle a script error.  Generally called by a script context.
   */
  virtual nsresult HandleScriptError(
                     mozilla::InternalScriptErrorEvent *aErrorEvent,
                     nsEventStatus *aEventStatus) {
    NS_ENSURE_STATE(NS_HandleScriptError(this, aErrorEvent, aEventStatus));
    return NS_OK;
  }

  virtual bool IsBlackForCC() { return false; }
};

NS_DEFINE_STATIC_IID_ACCESSOR(nsIScriptGlobalObject,
                              NS_ISCRIPTGLOBALOBJECT_IID)

#endif
