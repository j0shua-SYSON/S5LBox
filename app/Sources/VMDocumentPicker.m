// Copyright (c) 2026 j0shua-SYSON. MIT licensed.
#import "VMDocumentPicker.h"
#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>

UIDocumentPickerViewController *VMCreateImportPicker(void) {
    UIDocumentPickerViewController *picker;
    if (@available(iOS 14.0, *)) {
        picker = [[UIDocumentPickerViewController alloc]
            initForOpeningContentTypes:@[UTTypeData, UTTypeContent] asCopy:YES];
    } else {
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
        picker = [[UIDocumentPickerViewController alloc]
            initWithDocumentTypes:@[@"public.data", @"public.content"]
            inMode:UIDocumentPickerModeImport];
#pragma clang diagnostic pop
    }
    picker.allowsMultipleSelection = NO;
    return picker;
}
