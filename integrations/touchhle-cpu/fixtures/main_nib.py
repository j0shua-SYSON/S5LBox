# Copyright (c) 2026 j0shua-SYSON. MIT licensed.
"""Create small original keyed-archive nib fixtures without Interface Builder."""
import pathlib
import plistlib
import sys


def archive(path, main):
    objects = ["$null"]

    def add(value):
        objects.append(value)
        return plistlib.UID(len(objects) - 1)

    def instance(name, **fields):
        cls = add({"$classname": name, "$classes": [name, "NSObject"]})
        return add({"$class": cls, **fields})

    def array(values):
        return instance("NSArray", **{"NS.objects": values})

    if main:
        owner = instance("UIProxyObject", UIProxiedObjectIdentifier=add("IBFilesOwner"))
        delegate = instance("MainNibDelegate")
        window = instance("MainNibWindow", UIBounds=add("{{0, 0}, {320, 480}}"),
                          UICenter=add("{160, 240}"), UIOpaque=True)
        conns = [instance("UIRuntimeOutletConnection", UISource=owner,
                          UIDestination=delegate, UILabel=add("delegate")),
                 instance("UIRuntimeOutletConnection", UISource=delegate,
                          UIDestination=window, UILabel=add("window"))]
        all_objects, top, visible = [owner, delegate, window], [delegate, window], [window]
    else:
        token = instance("SecondaryNibToken")
        all_objects, top, visible, conns = [token], [token], [], []
    top_dict = {"UINibObjectsKey": array(all_objects),
                "UINibTopLevelObjectsKey": array(top),
                "UINibConnectionsKey": array(conns),
                "UINibVisibleWindowsKey": array(visible)}
    path.write_bytes(plistlib.dumps({"$archiver": "NSKeyedArchiver", "$version": 100000,
                                    "$objects": objects, "$top": top_dict}, fmt=plistlib.FMT_BINARY))


output = pathlib.Path(sys.argv[1])
output.mkdir(parents=True, exist_ok=True)
archive(output / "Main.nib", True)
archive(output / "Secondary.nib", False)
(output / "Info.plist").write_bytes(plistlib.dumps({
    "CFBundleExecutable": "InterpreterSmoke", "CFBundleIdentifier": "org.s5lbox.main-nib-fixture",
    "CFBundleName": "MainNibSmoke", "CFBundlePackageType": "APPL",
    "CFBundleVersion": "1", "MinimumOSVersion": "3.0", "NSMainNibFile": "Main",
}))
