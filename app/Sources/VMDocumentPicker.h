// Copyright (c) 2026 j0shua-SYSON. MIT licensed.
#import <UIKit/UIKit.h>

// These flows consume an archive/file; they do not edit a provider's original.
// Copy mode avoids the external-file access grant that can fail before the
// delegate is called in re-signed installations. Keep the iOS 13 fallback.
UIDocumentPickerViewController *VMCreateImportPicker(void);
