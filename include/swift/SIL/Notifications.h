//===--- Notifications.h - SIL Undef Value Representation -------*- C++ -*-===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2017 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//

#ifndef SWIFT_SIL_NOTIFICATIONS_H
#define SWIFT_SIL_NOTIFICATIONS_H

namespace swift {

class SILNode;

/// A protocol for handling SIL deserialization notifications.
class SILDeserializationNotificationHandler {
public:
  /// Observe that we deserialized a function declaration.
  virtual void didDeserialize(ModuleDecl *mod, SILFunction *fn) {}

  /// Observe that we successfully deserialized a function body.
  virtual void didDeserializeFunctionBody(ModuleDecl *mod, SILFunction *fn) {}

  /// Observe that we successfully deserialized a witness table's entries.
  virtual void didDeserializeWitnessTableEntries(ModuleDecl *mod,
                                                 SILWitnessTable *wt) {}

  /// Observe that we successfully deserialized a default witness table's
  /// entries.
  virtual void
  didDeserializeDefaultWitnessTableEntries(ModuleDecl *mod,
                                           SILDefaultWitnessTable *wt) {}

  /// Observe that we deserialized a global variable declaration.
  virtual void didDeserialize(ModuleDecl *mod, SILGlobalVariable *var) {}

  /// Observe that we deserialized a v-table declaration.
  virtual void didDeserialize(ModuleDecl *mod, SILVTable *vtable) {}

  /// Observe that we deserialized a witness-table declaration.
  virtual void didDeserialize(ModuleDecl *mod, SILWitnessTable *wtable) {}

  /// Observe that we deserialized a default witness-table declaration.
  virtual void didDeserialize(ModuleDecl *mod, SILDefaultWitnessTable *wtable) {
  }

  virtual ~SILDeserializationNotificationHandler() = default;

private:
  virtual void _anchor();
};

/// A notification handler that only overrides didDeserializeFunctionBody and
/// calls the passed in function pointer.
class SILFunctionBodyDeserializationNotificationHandler
    : public SILDeserializationNotificationHandler {
public:
  using Callback = void (*)(ModuleDecl *, SILFunction *);

private:
  Callback callback;

public:
  SimpleSILFunctionBodyDeserializationNotificationHandler(Callback callback)
      : callback(callback) {}

  void didDeserializeFunctionBody(ModuleDecl *mod, SILFunction *fn) override {
    (*callback)(mod, fn);
  }
};

/// A protocol (or interface) for handling value deletion notifications.
///
/// This class is used as a base class for any class that need to accept
/// instruction deletion notification messages. This is used by passes and
/// analysis that need to invalidate data structures that contain pointers.
/// This is similar to LLVM's ValueHandle.
struct DeleteNotificationHandler {
  DeleteNotificationHandler() { }
  virtual ~DeleteNotificationHandler() {}

  /// Handle the invalidation message for the value \p Value.
  virtual void handleDeleteNotification(SILNode *value) { }

  /// Returns True if the pass, analysis or other entity wants to receive
  /// notifications. This callback is called once when the class is being
  /// registered, and not once per notification. Entities that implement
  /// this callback should always return a constant answer (true/false).
  virtual bool needsNotifications() { return false; }
};

} // end swift namespace

#endif

