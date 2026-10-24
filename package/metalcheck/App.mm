#import <Foundation/Foundation.h>
#import "Core.h"
#include <stdio.h>
#include <cmath>

static void MCPrintUsage(FILE *stream, NSString *program) {
    const char *name = program.lastPathComponent.UTF8String;
    fprintf(stream, "usage: %s --cli [--library PATH] [--output PATH] [--benchmark | --stress-seconds 1..86400 [--memory-mib 0..512]]\n",
            name != NULL ? name : "metalcheck");
}

static NSString *MCDefaultLibraryPath(NSString *programPath) {
    NSString *executable = [NSBundle mainBundle].executablePath;
    if (executable.length == 0) {
        executable = programPath.length > 0 ? programPath : @"metalcheck";
        if (![executable isAbsolutePath]) {
            executable = [[[NSFileManager defaultManager] currentDirectoryPath] stringByAppendingPathComponent:executable];
        }
    }
    return [[[executable stringByStandardizingPath] stringByDeletingLastPathComponent]
        stringByAppendingPathComponent:@"default.metallib"];
}

static NSData *MCJSONData(NSDictionary *result, NSError **error) {
    NSData *json = [NSJSONSerialization dataWithJSONObject:result options:NSJSONWritingPrettyPrinted error:error];
    if (json == nil) {
        return nil;
    }
    NSMutableData *terminated = [json mutableCopy];
    const char newline = '\n';
    [terminated appendBytes:&newline length:1];
    return terminated;
}

static int MCCommandLineMain(NSArray<NSString *> *arguments) {
    BOOL cli = NO;
    BOOL benchmark = NO;
    double stressSeconds = 0;
    NSUInteger memoryMiB = 128;
    BOOL memorySpecified = NO;
    NSString *libraryPath = nil;
    NSString *outputPath = nil;
    for (NSUInteger index = 1; index < arguments.count; index++) {
        NSString *argument = arguments[index];
        if ([argument isEqualToString:@"--cli"]) {
            cli = YES;
        } else if ([argument isEqualToString:@"--benchmark"]) {
            benchmark = YES;
        } else if ([argument isEqualToString:@"--stress-seconds"] || [argument isEqualToString:@"--memory-mib"]) {
            if (index + 1 >= arguments.count) { MCPrintUsage(stderr, arguments.firstObject); return 2; }
            NSString *value = arguments[++index];
            NSScanner *scanner = [NSScanner scannerWithString:value];
            double number = 0;
            if (![scanner scanDouble:&number] || !scanner.isAtEnd || !std::isfinite(number)) {
                MCPrintUsage(stderr, arguments.firstObject); return 2;
            }
            if ([argument isEqualToString:@"--stress-seconds"]) {
                if (number < 1 || number > 86400) { MCPrintUsage(stderr, arguments.firstObject); return 2; }
                stressSeconds = number;
            } else {
                if (number < 0 || number > 512 || std::floor(number) != number) { MCPrintUsage(stderr, arguments.firstObject); return 2; }
                memoryMiB = (NSUInteger)number;
                memorySpecified = YES;
            }
        } else if ([argument isEqualToString:@"--library"] || [argument isEqualToString:@"--output"]) {
            if (index + 1 >= arguments.count) {
                MCPrintUsage(stderr, arguments.firstObject);
                return 2;
            }
            NSString *value = arguments[++index];
            if (value.length == 0) {
                MCPrintUsage(stderr, arguments.firstObject);
                return 2;
            }
            if ([argument isEqualToString:@"--library"]) {
                libraryPath = value;
            } else {
                outputPath = value;
            }
        } else {
            MCPrintUsage(stderr, arguments.firstObject);
            return 2;
        }
    }
    if (!cli || (benchmark && stressSeconds > 0) || (memorySpecified && stressSeconds == 0)) {
        MCPrintUsage(stderr, arguments.firstObject);
        return 2;
    }

    if (libraryPath == nil) {
        libraryPath = MCDefaultLibraryPath(arguments.firstObject);
    }
    NSDictionary *result = nil;
    @try {
        void (^printProgress)(NSString *) = ^(NSString *progress) {
            const char *line = progress.UTF8String;
            fprintf(stderr, "%s\n", line != NULL ? line : "");
            fflush(stderr);
        };
        result = stressSeconds > 0 ? MCRunStress(libraryPath, stressSeconds, memoryMiB, nil, printProgress)
                                   : MCRun(libraryPath, benchmark, printProgress);
    } @catch (NSException *exception) {
        NSString *reason = exception.reason != nil ? exception.reason : exception.name;
        result = @{@"passed": @NO,
                   @"error": reason != nil ? reason : @"Metal Check raised an exception"};
    }
    if (result == nil) {
        result = @{@"passed": @NO, @"error": @"Metal Check returned no result"};
    }

    NSError *jsonError = nil;
    NSData *json = MCJSONData(result, &jsonError);
    if (json == nil) {
        const char *message = jsonError.localizedDescription.UTF8String;
        fprintf(stderr, "could not encode result as JSON: %s\n", message != NULL ? message : "unknown error");
        return 1;
    }
    (void)fwrite(json.bytes, 1, json.length, stdout);
    fflush(stdout);

    BOOL outputOK = YES;
    if (outputPath != nil) {
        NSString *directory = [outputPath stringByDeletingLastPathComponent];
        if (directory.length == 0) {
            directory = @".";
        }
        NSError *directoryError = nil;
        if (![[NSFileManager defaultManager] createDirectoryAtPath:directory
                                      withIntermediateDirectories:YES
                                                       attributes:nil
                                                            error:&directoryError]) {
            const char *message = directoryError.localizedDescription.UTF8String;
            fprintf(stderr, "could not create output directory: %s\n", message != NULL ? message : "unknown error");
            outputOK = NO;
        } else {
            NSError *writeError = nil;
            if (![json writeToFile:outputPath options:NSDataWritingAtomic error:&writeError]) {
                const char *message = writeError.localizedDescription.UTF8String;
                fprintf(stderr, "could not write output file: %s\n", message != NULL ? message : "unknown error");
                outputOK = NO;
            }
        }
    }

    id passedValue = result[@"passed"];
    BOOL passed = [passedValue respondsToSelector:@selector(boolValue)] && [passedValue boolValue];
    return passed && outputOK ? 0 : 1;
}

#if defined(METALCHECK_APP) && METALCHECK_APP

#import <UIKit/UIKit.h>

@interface MCViewController : UIViewController
@end

@interface MCViewController ()
@property(nonatomic, strong) UILabel *headingLabel;
@property(nonatomic, strong) UIButton *correctnessButton;
@property(nonatomic, strong) UIButton *performanceButton;
@property(nonatomic, strong) UIButton *stressButton;
@property(nonatomic, strong) UIButton *stopButton;
@property(nonatomic, strong) UISegmentedControl *memoryControl;
@property(atomic) BOOL stopRequested;
@property(nonatomic, strong) UITextView *outputView;
@property(nonatomic, strong) dispatch_queue_t testQueue;
@end

@implementation MCViewController

- (void)viewDidLoad {
    [super viewDidLoad];
    self.view.backgroundColor = UIColor.systemBackgroundColor;
    self.testQueue = dispatch_queue_create("org.droidvm.metalcheck.tests", DISPATCH_QUEUE_SERIAL);

    self.headingLabel = [[UILabel alloc] init];
    self.headingLabel.text = @"Metal Check";
    self.headingLabel.font = [UIFont preferredFontForTextStyle:UIFontTextStyleLargeTitle];
    self.headingLabel.adjustsFontForContentSizeCategory = YES;

    self.correctnessButton = [UIButton buttonWithType:UIButtonTypeSystem];
    [self.correctnessButton setTitle:@"Correctness" forState:UIControlStateNormal];
    [self.correctnessButton addTarget:self action:@selector(runCorrectness:) forControlEvents:UIControlEventTouchUpInside];

    self.performanceButton = [UIButton buttonWithType:UIButtonTypeSystem];
    [self.performanceButton setTitle:@"Correctness + Performance" forState:UIControlStateNormal];
    [self.performanceButton addTarget:self action:@selector(runPerformance:) forControlEvents:UIControlEventTouchUpInside];

    self.stressButton = [UIButton buttonWithType:UIButtonTypeSystem];
    [self.stressButton setTitle:@"GPU Stress — 5 minutes" forState:UIControlStateNormal];
    [self.stressButton addTarget:self action:@selector(runStress:) forControlEvents:UIControlEventTouchUpInside];
    self.memoryControl = [[UISegmentedControl alloc] initWithItems:@[@"+0 MiB", @"+128 MiB", @"+256 MiB"]];
    self.memoryControl.selectedSegmentIndex = 1;
    self.memoryControl.accessibilityLabel = @"Extra resident memory during GPU stress";
    self.stopButton = [UIButton buttonWithType:UIButtonTypeSystem];
    [self.stopButton setTitle:@"Stop after current round" forState:UIControlStateNormal];
    self.stopButton.enabled = NO;
    [self.stopButton addTarget:self action:@selector(stopStress:) forControlEvents:UIControlEventTouchUpInside];
    [[NSNotificationCenter defaultCenter] addObserver:self selector:@selector(stopStress:)
                                                 name:UIApplicationDidEnterBackgroundNotification object:nil];

    self.outputView = [[UITextView alloc] init];
    self.outputView.editable = NO;
    self.outputView.selectable = YES;
    self.outputView.font = [UIFont monospacedSystemFontOfSize:13.0 weight:UIFontWeightRegular];
    self.outputView.backgroundColor = UIColor.secondarySystemBackgroundColor;
    self.outputView.layer.cornerRadius = 8.0;
    self.outputView.accessibilityLabel = @"Metal Check output";

    UIStackView *stack = [[UIStackView alloc] initWithArrangedSubviews:@[
        self.headingLabel, self.correctnessButton, self.performanceButton, self.stressButton,
        self.memoryControl, self.stopButton, self.outputView
    ]];
    stack.axis = UILayoutConstraintAxisVertical;
    stack.spacing = 8.0;
    stack.translatesAutoresizingMaskIntoConstraints = NO;
    [self.view addSubview:stack];

    UILayoutGuide *safeArea = self.view.safeAreaLayoutGuide;
    [NSLayoutConstraint activateConstraints:@[
        [stack.leadingAnchor constraintEqualToAnchor:safeArea.leadingAnchor constant:16.0],
        [stack.trailingAnchor constraintEqualToAnchor:safeArea.trailingAnchor constant:-16.0],
        [stack.topAnchor constraintEqualToAnchor:safeArea.topAnchor constant:8.0],
        [stack.bottomAnchor constraintEqualToAnchor:safeArea.bottomAnchor constant:-8.0],
        [self.outputView.heightAnchor constraintGreaterThanOrEqualToConstant:100.0]
    ]];
}

- (void)appendOutput:(NSString *)line {
    NSString *current = self.outputView.text != nil ? self.outputView.text : @"";
    NSString *message = line != nil ? line : @"";
    self.outputView.text = [current stringByAppendingFormat:@"%@\n", message];
    [self.outputView scrollRangeToVisible:NSMakeRange(self.outputView.text.length, 0)];
}

- (void)runCorrectness:(id)sender {
    (void)sender;
    [self runWithBenchmark:NO stress:NO];
}

- (void)runPerformance:(id)sender {
    (void)sender;
    [self runWithBenchmark:YES stress:NO];
}

- (void)runStress:(id)sender {
    (void)sender;
    [self runWithBenchmark:NO stress:YES];
}

- (void)stopStress:(id)sender {
    (void)sender;
    self.stopRequested = YES;
    self.stopButton.enabled = NO;
}

- (void)runWithBenchmark:(BOOL)benchmark stress:(BOOL)stress {
    self.correctnessButton.enabled = NO;
    self.performanceButton.enabled = NO;
    self.stressButton.enabled = NO;
    self.memoryControl.enabled = NO;
    self.stopRequested = NO;
    self.stopButton.enabled = stress;
    UIApplication.sharedApplication.idleTimerDisabled = YES;
    NSUInteger memoryMiB = self.memoryControl.selectedSegmentIndex == 2 ? 256 : self.memoryControl.selectedSegmentIndex == 1 ? 128 : 0;
    self.outputView.text = @"Running Metal checks…\n";

    NSString *libraryPath = [[NSBundle mainBundle] pathForResource:@"default" ofType:@"metallib"];
    if (libraryPath == nil) {
        libraryPath = @"";
    }
    dispatch_async(self.testQueue, ^{
        NSDictionary *result = nil;
        @try {
            void (^showProgress)(NSString *) = ^(NSString *progress) {
                dispatch_async(dispatch_get_main_queue(), ^{
                    [self appendOutput:progress];
                });
            };
            result = stress ? MCRunStress(libraryPath, 300, memoryMiB, ^bool { return self.stopRequested; }, showProgress)
                            : MCRun(libraryPath, benchmark, showProgress);
        } @catch (NSException *exception) {
            NSString *reason = exception.reason != nil ? exception.reason : exception.name;
            result = @{@"passed": @NO,
                       @"error": reason != nil ? reason : @"Metal Check raised an exception"};
        }
        if (result == nil) {
            result = @{@"passed": @NO, @"error": @"Metal Check returned no result"};
        }

        NSError *jsonError = nil;
        NSData *jsonData = [NSJSONSerialization dataWithJSONObject:result
                                                           options:NSJSONWritingPrettyPrinted
                                                             error:&jsonError];
        NSString *output = jsonData != nil ? [[NSString alloc] initWithData:jsonData encoding:NSUTF8StringEncoding] : nil;
        NSString *saveErrorText = nil;
        if (jsonData != nil) {
            NSArray<NSString *> *documentPaths = NSSearchPathForDirectoriesInDomains(NSDocumentDirectory, NSUserDomainMask, YES);
            NSString *documents = documentPaths.firstObject;
            if (documents == nil) {
                saveErrorText = @"Could not locate the Documents directory";
            } else {
                NSString *resultDirectory = [documents stringByAppendingPathComponent:@"MetalCheck"];
                NSError *directoryError = nil;
                if ([[NSFileManager defaultManager] createDirectoryAtPath:resultDirectory
                                             withIntermediateDirectories:YES
                                                              attributes:nil
                                                                   error:&directoryError]) {
                    NSString *resultPath = [resultDirectory stringByAppendingPathComponent:@"latest.json"];
                    if (![jsonData writeToFile:resultPath options:NSDataWritingAtomic error:&directoryError]) {
                        saveErrorText = directoryError.localizedDescription != nil
                            ? directoryError.localizedDescription : @"Could not write latest.json";
                    }
                } else {
                    saveErrorText = directoryError.localizedDescription != nil
                        ? directoryError.localizedDescription : @"Could not create the result directory";
                }
            }
        } else {
            saveErrorText = jsonError.localizedDescription != nil
                ? jsonError.localizedDescription : @"Could not encode the result as JSON";
        }

        NSString *finalOutput = output != nil ? output : @"{\n  \"passed\": false,\n  \"error\": \"Could not encode the result as JSON\"\n}";
        dispatch_async(dispatch_get_main_queue(), ^{
            if (saveErrorText != nil) {
                [self appendOutput:[@"Report save error: " stringByAppendingString:saveErrorText]];
            }
            [self appendOutput:finalOutput];
            self.correctnessButton.enabled = YES;
            self.performanceButton.enabled = YES;
            self.stressButton.enabled = YES;
            self.memoryControl.enabled = YES;
            self.stopButton.enabled = NO;
            UIApplication.sharedApplication.idleTimerDisabled = NO;
        });
    });
}

@end

@interface MCAppDelegate : UIResponder <UIApplicationDelegate>
@property(nonatomic, strong) UIWindow *window;
@end

@implementation MCAppDelegate

- (BOOL)application:(UIApplication *)application didFinishLaunchingWithOptions:(NSDictionary *)launchOptions {
    (void)application;
    (void)launchOptions;
    self.window = [[UIWindow alloc] initWithFrame:UIScreen.mainScreen.bounds];
    self.window.rootViewController = [[MCViewController alloc] init];
    [self.window makeKeyAndVisible];
    return YES;
}

@end

int main(int argc, char *argv[]) {
    @autoreleasepool {
        NSArray<NSString *> *arguments = NSProcessInfo.processInfo.arguments;
        if ([arguments containsObject:@"--cli"]) {
            return MCCommandLineMain(arguments);
        }
        return UIApplicationMain(argc, argv, nil, NSStringFromClass([MCAppDelegate class]));
    }
}

#else

int main(int argc, const char *argv[]) {
    @autoreleasepool {
        (void)argc;
        (void)argv;
        return MCCommandLineMain(NSProcessInfo.processInfo.arguments);
    }
}

#endif
