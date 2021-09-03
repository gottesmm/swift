#ifndef SWIFT_IDE_TEST_MODULE_API_DIFF_H
#define SWIFT_IDE_TEST_MODULE_API_DIFF_H

#include "swift/Basic/LLVM.h"
#include "llvm/ADT/Optional.h"
#include <string>

namespace swift {

int doGenerateModuleAPIDescription(StringRef MainExecutablePath,
                                   ArrayRef<std::string> Args,
                                   Optional<StringRef> RuntimeResourceDir);

} // end namespace swift

#endif // SWIFT_IDE_TEST_MODULE_API_DIFF_H

