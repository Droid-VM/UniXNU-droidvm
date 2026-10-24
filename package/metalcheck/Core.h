#import <Foundation/Foundation.h>
#include <stdbool.h>

FOUNDATION_EXPORT NSDictionary *MCRun(NSString *libraryPath, bool benchmark, void (^progress)(NSString *));
FOUNDATION_EXPORT NSDictionary *MCRunStress(NSString *libraryPath, double seconds, NSUInteger memoryMiB,
                                            bool (^shouldStop)(void), void (^progress)(NSString *));
