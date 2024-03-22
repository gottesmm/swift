
// Make sure that we have set the -D flag appropriately.
#ifdef __SWIFT_ATTR_SUPPORTS_SENDABLE_DECLS
#if !__SWIFT_ATTR_SUPPORTS_SENDABLE_DECLS
#error "Compiler should have set __SWIFT_ATTR_SUPPORTS_SENDABLE_DECLS to 1"
#endif
#else
#error "Compiler should have defined __SWIFT_ATTR_SUPPORTS_SENDABLE_DECLS"
#endif

@import Foundation;

#pragma clang assume_nonnull begin

#define SWIFT_TRANSFERRING __attribute__((swift_attr("transferring")))

@interface MyType : NSObject
- (NSObject *)getTransferringResult SWIFT_TRANSFERRING;
- (NSObject *)getTransferringResultWithArgument:(NSObject *)arg SWIFT_TRANSFERRING;
- (NSObject *)getResultWithTransferringArgument:(NSObject *) SWIFT_TRANSFERRING arg;
@end

SWIFT_TRANSFERRING
@interface DoesntMakeSense : NSObject
@end

NSObject *testCallGlobalWithResult(NSObject *other);
NSObject *testCallGlobalWithTransferringResult(NSObject *other) SWIFT_TRANSFERRING;
void testCallGlobalWithTransferringArg(NSObject * arg SWIFT_TRANSFERRING);

#pragma clang assume_nonnull end
