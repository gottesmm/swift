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

#include "swift/Basic/LLVM.h"
#include "swift/Basic/STLExtras.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/PointerUnion.h"
#include <memory>

namespace swift {

class SILNode;
class ModuleDecl;
class SILFunction;
class SILWitnessTable;
class SILDefaultWitnessTable;
class SILGlobalVariable;
class SILVTable;
class SILPassManager;

/// An abstract class for handling SIL deserialization notifications.
///
/// This is an abstract class since we want to separate having notifications
/// from being able to be used within DeserializationNotificationHandlerSet
/// since otherwise we could pass DeserializationNotificationHandlerSet into
/// itself.
class DeserializationNotificationHandlerBase {
public:
  /// Observe that we deserialized a function declaration.
  virtual void didDeserialize(ModuleDecl *mod, SILFunction *fn) = 0;

  /// Observe that we successfully deserialized a function body.
  virtual void didDeserializeFunctionBody(ModuleDecl *mod, SILFunction *fn) = 0;

  /// Observe that we successfully deserialized a witness table's entries.
  virtual void didDeserializeWitnessTableEntries(ModuleDecl *mod,
                                                 SILWitnessTable *wt) = 0;

  /// Observe that we successfully deserialized a default witness table's
  /// entries.
  virtual void
  didDeserializeDefaultWitnessTableEntries(ModuleDecl *mod,
                                           SILDefaultWitnessTable *wt) = 0;

  /// Observe that we deserialized a global variable declaration.
  virtual void didDeserialize(ModuleDecl *mod, SILGlobalVariable *var) = 0;

  /// Observe that we deserialized a v-table declaration.
  virtual void didDeserialize(ModuleDecl *mod, SILVTable *vtable) = 0;

  /// Observe that we deserialized a witness-table declaration.
  virtual void didDeserialize(ModuleDecl *mod, SILWitnessTable *wtable) = 0;

  /// Observe that we deserialized a default witness-table declaration.
  virtual void didDeserialize(ModuleDecl *mod, SILDefaultWitnessTable *wtable) = 0;

  virtual ~DeserializationNotificationHandlerBase() = default;
};

/// A no-op implementation of DeserializationNotificationHandlerBase. Intended
/// to allow for users to implement only one of the relevant methods and have
/// all other methods be no-ops.
class DeserializationNotificationHandler :
    public DeserializationNotificationHandlerBase {
public:
  /// Observe that we deserialized a function declaration.
  virtual void didDeserialize(ModuleDecl *mod, SILFunction *fn) override {}

  /// Observe that we successfully deserialized a function body.
  virtual void didDeserializeFunctionBody(ModuleDecl *mod, SILFunction *fn) override {}

  /// Observe that we successfully deserialized a witness table's entries.
  virtual void didDeserializeWitnessTableEntries(ModuleDecl *mod,
                                                 SILWitnessTable *wt) override {}

  /// Observe that we successfully deserialized a default witness table's
  /// entries.
  virtual void
  didDeserializeDefaultWitnessTableEntries(ModuleDecl *mod,
                                           SILDefaultWitnessTable *wt) override {}

  /// Observe that we deserialized a global variable declaration.
  virtual void didDeserialize(ModuleDecl *mod, SILGlobalVariable *var) override {}

  /// Observe that we deserialized a v-table declaration.
  virtual void didDeserialize(ModuleDecl *mod, SILVTable *vtable) override {}

  /// Observe that we deserialized a witness-table declaration.
  virtual void didDeserialize(ModuleDecl *mod, SILWitnessTable *wtable) override {}

  /// Observe that we deserialized a default witness-table declaration.
  virtual void didDeserialize(ModuleDecl *mod, SILDefaultWitnessTable *wtable) override {
  }

  virtual ~DeserializationNotificationHandler() = default;
};

/// A notification handler that only overrides didDeserializeFunctionBody and
/// calls the passed in function pointer.
class FunctionBodyDeserializationNotificationHandler
    : public DeserializationNotificationHandler {
public:
  using Handler = void (*)(ModuleDecl *, SILFunction *);

private:
  Handler handler;

public:
  FunctionBodyDeserializationNotificationHandler(Handler handler)
      : handler(handler) {}
  virtual ~FunctionBodyDeserializationNotificationHandler() {}

  void didDeserializeFunctionBody(ModuleDecl *mod, SILFunction *fn) override {
    (*handler)(mod, fn);
  }
};

/// A type that enables us to both pass in custom notification handler types
/// that use a unique_ptr as well as pass managers and analyses that we want
/// to pass in a bare pointer for. We use the API
class DeserializationNotificationHandlerSet : public DeserializationNotificationHandlerBase {
public:
  /// A value that is either functionally a std::unique_ptr or an unsafe unowned
  /// pointer. This is implemented by setting a bit in the tagged bits that says
  /// whether the object must be deleted. In other words the encoding is:
  ///
  ///   true => delete,
  ///   false => no-delete.
  ///
  ///
  /// This is implemented in this manner since std::unique_ptr doesn't have
  /// pointer like type traits and thus can not be used in a PointerUnion. This
  /// is not pretty, but it gets the job done.
  using HandlerTy =
    llvm::PointerIntPair<DeserializationNotificationHandler *, 1>;

private:
  /// A list of deserialization callbacks that update the SILModule and other
  /// parts of SIL as deserialization occurs.
  ///
  /// We use 2 here since that is the most that will ever be used today in the
  /// compiler. If that changed, that number should be changed as well.
  SmallVector<HandlerTy, 2> handlerSet;

public:
  DeserializationNotificationHandlerSet() = default;
  ~DeserializationNotificationHandlerSet() {
    for (auto &h : handlerSet) {
      // If getInt is true, then we need to delete the pointer.
      if (h.getInt()) {
        delete h.getPointer();
      }
    }
  }

  void erase(DeserializationNotificationHandler *handler) {
    auto iter = find_if(handlerSet,
                        [&](HandlerTy &h) { return handler == h.getPointer(); });
    if (iter == handlerSet.end())
      return;

    if (iter->getInt())
      delete iter->getPointer();

    handlerSet.erase(iter);
  }

  void add(std::unique_ptr<DeserializationNotificationHandler> &&handler) {
    handlerSet.emplace_back(handler.release(), true/*should delete*/);
    sortUnique(handlerSet);
  }

  /// Add a pass manager pointer to the callback set as a bare pointer. The
  /// key thing here is that we never dereference the pointer, so we do not
  /// need the full definition of SILPassManager here. Otherwise we would have
  /// a layering violation.
  void add(SILPassManager *pm) {
    auto *h = reinterpret_cast<DeserializationNotificationHandler *>(pm);
    handlerSet.emplace_back(h, false/*should delete*/);
    sortUnique(handlerSet);
  }

  static DeserializationNotificationHandler *getUnderlyingHandler(const HandlerTy &h) {
    return h.getPointer();
  }

  using iterator = llvm::mapped_iterator<
    decltype(handlerSet)::const_iterator,
    decltype(&DeserializationNotificationHandlerSet::getUnderlyingHandler)
  >;

  iterator begin() const {
    auto *fptr = &DeserializationNotificationHandlerSet::getUnderlyingHandler;
    return llvm::map_iterator(handlerSet.begin(), fptr);
  }

  iterator end() const {
    auto *fptr = &DeserializationNotificationHandlerSet::getUnderlyingHandler;
    return llvm::map_iterator(handlerSet.end(), fptr);
  }

  using range = llvm::iterator_range<iterator>;
  auto getRange() const -> range {
    return llvm::make_range(begin(), end());
  }

  // DeserializationNotificationHandler implementation via chaining to the
  // handlers we contain.
#ifdef DNS_CHAIN_METHOD
#error "DNS_CHAIN_METHOD is defined?!"
#endif
#define DNS_CHAIN_METHOD(Name, FirstTy, SecondTy)              \
  void did##Name(FirstTy first, SecondTy second) override {    \
    for (auto *p : getRange()) {                               \
      p->did##Name(first, second);                             \
    }                                                          \
  }
  DNS_CHAIN_METHOD(Deserialize, ModuleDecl *, SILFunction *)
  DNS_CHAIN_METHOD(DeserializeFunctionBody, ModuleDecl *, SILFunction *)
  DNS_CHAIN_METHOD(DeserializeWitnessTableEntries, ModuleDecl *,
                   SILWitnessTable *)
  DNS_CHAIN_METHOD(DeserializeDefaultWitnessTableEntries, ModuleDecl *,
                   SILDefaultWitnessTable *)
  DNS_CHAIN_METHOD(Deserialize, ModuleDecl *, SILGlobalVariable *)
  DNS_CHAIN_METHOD(Deserialize, ModuleDecl *, SILVTable *)
  DNS_CHAIN_METHOD(Deserialize, ModuleDecl *, SILWitnessTable *)
  DNS_CHAIN_METHOD(Deserialize, ModuleDecl *, SILDefaultWitnessTable *)
#undef DNS_CHAIN_METHOD
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

} // swift namespace

#endif

