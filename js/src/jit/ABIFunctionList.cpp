#include "jit/ABIFunctionList-inl.h"

namespace js::jit {

#ifdef JS_SPASM
#define DEF_TEMPLATE(sig, ...) \
  ABIFunctionSignatureMap<sig> ABIFunctionSignatureData<sig>::lookupSym = { \
    __VA_ARGS__ \
  };
ABIFUNCTIONSIG_AND_SYMBOL_LIST(DEF_TEMPLATE)
#undef DEF_TEMPLATE
#endif

}
