#ifndef __OS_FUNCTIONS_H_
#define __OS_FUNCTIONS_H_

// RegionFix build: use the installed WUT IOS declarations. The legacy
// redeclarations below disagreed with current enum and const parameter types.
#include <coreinit/ios.h>

#ifdef __cplusplus
extern "C" {
#endif

#define OS_MUTEX_SIZE 44

//!----------------------------------------------------------------------------------------------------------------------------------------------------------------------------
//! Mutex functions
//!----------------------------------------------------------------------------------------------------------------------------------------------------------------------------
extern void OSInitMutex(void *mutex);

extern void OSLockMutex(void *mutex);

extern void OSUnlockMutex(void *mutex);

//!----------------------------------------------------------------------------------------------------------------------------------------------------------------------------
//! IOS function
//!----------------------------------------------------------------------------------------------------------------------------------------------------------------------------

#ifdef __cplusplus
}
#endif

#endif // __OS_FUNCTIONS_H_
