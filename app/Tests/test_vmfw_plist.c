/*
 * S5LBox -- tests for the XML property list scanner.
 *
 * Two kinds of input are exercised, and the split is deliberate. The first is
 * the real Restore.plist out of iPhone1,2_3.1.3_7E18_Restore.ipsw, embedded
 * byte for byte, because a scanner that passes on documents we wrote is only
 * evidence about documents we wrote. The second is everything an attacker or a
 * half-downloaded file can produce: unterminated tags, unbalanced containers,
 * keys with no value, nesting past the cap, entities that name nothing, and
 * every prefix of both real documents.
 *
 * The truncation sweep is exhaustive rather than sampled -- every prefix length
 * of both embedded documents is fed through init and all four accessors. It is
 * cheap, and it is the check that actually stands behind "a malformed plist
 * produces an error, never a read past the end".
 *
 * Copyright (c) 2026 j0shua-SYSON. MIT licensed.
 */
#include "VMFirmwareTest.h"
#include "VMFirmwareFormats.h"

#include <stddef.h>
#include <stdint.h>

/* ------------------------------------------------------------------------ */
/* Fixtures                                                                  */
/* ------------------------------------------------------------------------ */

/*
 * Restore.plist, verbatim from the 7E18 IPSW: 1,804 bytes of member names and
 * version strings. It carries no firmware, no key material and no payload, so
 * embedding it costs nothing and buys a test that fails if the scanner ever
 * stops reading the one manifest the importer cannot do without.
 */
static const char k_restore[] =
    "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
    "<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" \"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n"
    "<plist version=\"1.0\">\n"
    "<dict>\n"
    "\t<key>DeviceClass</key>\n"
    "\t<string>iPhone</string>\n"
    "\t<key>DeviceMap</key>\n"
    "\t<array>\n"
    "\t\t<dict>\n"
    "\t\t\t<key>BDID</key>\n"
    "\t\t\t<integer>4</integer>\n"
    "\t\t\t<key>BoardConfig</key>\n"
    "\t\t\t<string>n82ap</string>\n"
    "\t\t\t<key>CPID</key>\n"
    "\t\t\t<integer>35072</integer>\n"
    "\t\t\t<key>Platform</key>\n"
    "\t\t\t<string>s5l8900x</string>\n"
    "\t\t\t<key>SCEP</key>\n"
    "\t\t\t<integer>5</integer>\n"
    "\t\t</dict>\n"
    "\t</array>\n"
    "\t<key>FirmwareDirectory</key>\n"
    "\t<string>Firmware</string>\n"
    "\t<key>KernelCachesByPlatform</key>\n"
    "\t<dict>\n"
    "\t\t<key>s5l8900x</key>\n"
    "\t\t<dict>\n"
    "\t\t\t<key>Release</key>\n"
    "\t\t\t<string>kernelcache.release.s5l8900x</string>\n"
    "\t\t</dict>\n"
    "\t</dict>\n"
    "\t<key>ProductBuildVersion</key>\n"
    "\t<string>7E18</string>\n"
    "\t<key>ProductType</key>\n"
    "\t<string>iPhone1,2</string>\n"
    "\t<key>ProductVersion</key>\n"
    "\t<string>3.1.3</string>\n"
    "\t<key>RamDisksByPlatform</key>\n"
    "\t<dict>\n"
    "\t\t<key>s5l8900x</key>\n"
    "\t\t<dict>\n"
    "\t\t\t<key>Update</key>\n"
    "\t\t\t<string>018-6488-015.dmg</string>\n"
    "\t\t\t<key>User</key>\n"
    "\t\t\t<string>018-6494-014.dmg</string>\n"
    "\t\t</dict>\n"
    "\t</dict>\n"
    "\t<key>RestoreKernelCaches</key>\n"
    "\t<dict>\n"
    "\t\t<key>Release</key>\n"
    "\t\t<string>kernelcache.release.s5l8900x</string>\n"
    "\t</dict>\n"
    "\t<key>RestoreRamDisks</key>\n"
    "\t<dict>\n"
    "\t\t<key>Update</key>\n"
    "\t\t<string>018-6488-015.dmg</string>\n"
    "\t\t<key>User</key>\n"
    "\t\t<string>018-6494-014.dmg</string>\n"
    "\t</dict>\n"
    "\t<key>SupportedProductTypeIDs</key>\n"
    "\t<dict>\n"
    "\t\t<key>DFU</key>\n"
    "\t\t<array>\n"
    "\t\t\t<integer>304218112</integer>\n"
    "\t\t\t<integer>304230656</integer>\n"
    "\t\t</array>\n"
    "\t\t<key>Recovery</key>\n"
    "\t\t<array>\n"
    "\t\t\t<integer>310391040</integer>\n"
    "\t\t</array>\n"
    "\t</dict>\n"
    "\t<key>SupportedProductTypes</key>\n"
    "\t<array>\n"
    "\t\t<string>iPhone1,2</string>\n"
    "\t</array>\n"
    "\t<key>SystemRestoreImages</key>\n"
    "\t<dict>\n"
    "\t\t<key>User</key>\n"
    "\t\t<string>018-6482-014.dmg</string>\n"
    "\t</dict>\n"
    "</dict>\n"
    "</plist>\n";

/*
 * A UDIF resource fork. Synthetic, because the real one lives inside an
 * encrypted disk image, but generated to Apple's own layout: keys in sorted
 * order, tab indentation, and a <data> body whose lines are wrapped to
 * 76 - 8*indent characters. That width was measured rather than guessed: the 40
 * multi-line <data> blocks in the same IPSW's BuildManifesto.plist all sit at
 * five tabs and wrap at 36, and these sit at four tabs and wrap at 44. Nothing
 * in the decoder depends on the width either way -- see k_wide_data.
 *
 * Each blob is the pattern in fork_blob() so every byte of every entry can be
 * checked, not just the length.
 */
static const char k_fork[] =
    "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
    "<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" \"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n"
    "<plist version=\"1.0\">\n"
    "<dict>\n"
    "\t<key>resource-fork</key>\n"
    "\t<dict>\n"
    "\t\t<key>blkx</key>\n"
    "\t\t<array>\n"
    "\t\t\t<dict>\n"
    "\t\t\t\t<key>Attributes</key>\n"
    "\t\t\t\t<string>0x0050</string>\n"
    "\t\t\t\t<key>Data</key>\n"
    "\t\t\t\t<data>\n"
    "\t\t\t\tERgfJi00O0JJUFdeZWxzeoGIj5adpKuyucDHztXc4+rx\n"
    "\t\t\t\t+P8GDRQbIg==\n"
    "\t\t\t\t</data>\n"
    "\t\t\t\t<key>ID</key>\n"
    "\t\t\t\t<string>-1</string>\n"
    "\t\t\t\t<key>Name</key>\n"
    "\t\t\t\t<string>Driver Descriptor Map (DDM : 0)</string>\n"
    "\t\t\t</dict>\n"
    "\t\t\t<dict>\n"
    "\t\t\t\t<key>Attributes</key>\n"
    "\t\t\t\t<string>0x0000</string>\n"
    "\t\t\t\t<key>Data</key>\n"
    "\t\t\t\t<data>\n"
    "\t\t\t\tIikwNz5FTFNaYWhvdn2Ei5KZoKeutbzDytHY3+bt9PsC\n"
    "\t\t\t\tCRAXHiUsMzpBSE9WXWRrcnmAh46VnKOqsbi/xs3U2+Lp\n"
    "\t\t\t\t8Pf+BQwTGiEoLzY9REtSWWBnbnV8g4qRmJ+mrbS7wsnQ\n"
    "\t\t\t\t197l7PP6AQgPFh0kKzI5QEdOVVxjanF4f4aNlJuiqbC3\n"
    "\t\t\t\t</data>\n"
    "\t\t\t\t<key>ID</key>\n"
    "\t\t\t\t<string>0</string>\n"
    "\t\t\t\t<key>Name</key>\n"
    "\t\t\t\t<string>Apple (Apple_partition_map : 1)</string>\n"
    "\t\t\t</dict>\n"
    "\t\t\t<dict>\n"
    "\t\t\t\t<key>Attributes</key>\n"
    "\t\t\t\t<string>0x0000</string>\n"
    "\t\t\t\t<key>Data</key>\n"
    "\t\t\t\t<data>\n"
    "\t\t\t\tMzpBSE9WXWRrcnmAh46VnKOqsbi/xs3U2+Lp8Pf+BQwT\n"
    "\t\t\t\t</data>\n"
    "\t\t\t\t<key>ID</key>\n"
    "\t\t\t\t<string>1</string>\n"
    "\t\t\t\t<key>Name</key>\n"
    "\t\t\t\t<string>Macintosh (Apple_Driver_ATAPI : 2)</string>\n"
    "\t\t\t</dict>\n"
    "\t\t\t<dict>\n"
    "\t\t\t\t<key>Attributes</key>\n"
    "\t\t\t\t<string>0x0000</string>\n"
    "\t\t\t\t<key>Data</key>\n"
    "\t\t\t\t<data>\n"
    "\t\t\t\tREtSWWBnbnV8g4qRmJ+mrbS7wsnQ197l7PP6AQgPFh0k\n"
    "\t\t\t\tKzI5QEdOVVxjanF4f4aNlJuiqbC3vsXM09rh6O/2/QQL\n"
    "\t\t\t\tEhkgJy41PENKUVhfZm10e4KJkJeepayzusHIz9bd5Ovy\n"
    "\t\t\t\t+QAHDhUcIyoxOD9GTVRbYmlwd36FjJOaoaivtr3Ey9LZ\n"
    "\t\t\t\t4Ofu9fwDChEYHyYtNDtCSVBXXmVsc3qBiI+WnaSrsrnA\n"
    "\t\t\t\tx87V3OPq8fj/Bg0UGyIpMDc+RUxTWmFob3Z9hIuSmaCn\n"
    "\t\t\t\trrW8w8rR2N/m7fT7AgkQFx4lLDM6QUhPVl1ka3J5gIeO\n"
    "\t\t\t\tlZyjqrG4v8bN1Nvi6fD3/gUMExohKC82PURLUllgZ251\n"
    "\t\t\t\tfIOKkZifpq20u8LJ0Nfe5ezz+gEIDxYdJCsyOUBHTlVc\n"
    "\t\t\t\tY2pxeH+GjZSboqmwt77FzNPa4ejv9v0ECxIZICcuNTxD\n"
    "\t\t\t\tSlFYX2ZtdHuCiZCXnqWss7rByM/W3eTr8vkABw4VHCMq\n"
    "\t\t\t\tMTg/Rk1UW2JpcHd+hYyTmqGor7a9xMvS2eDn7vX8AwoR\n"
    "\t\t\t\tGB8mLTQ7QklQV15lbHN6gYiPlp2kq7K5wMfO1dzj6vH4\n"
    "\t\t\t\t/wYNFBsiKTA3PkVMU1phaG92fYSLkpmgp661vMPK0djf\n"
    "\t\t\t\t5u30+wIJEBceJSwzOkFIT1ZdZGtyeYCHjpWco6qxuL/G\n"
    "\t\t\t\tzdTb4unw9/4FDBMaISgvNj1ES1JZYGdudXyDipGYn6at\n"
    "\t\t\t\ttLvCydDX3uXs8/oBCA8WHSQrMjlAR05VXGNqcXh/ho2U\n"
    "\t\t\t\tm6KpsLe+xczT2uHo7/b9BAsSGSAnLjU8Q0pRWF9mbXR7\n"
    "\t\t\t\tgomQl56lrLO6wcjP1t3k6/L5AAcOFRwjKjE4P0ZNVFti\n"
    "\t\t\t\taXB3foWMk5qhqK+2vcTL0tng5+71/AMKERgfJi00O0JJ\n"
    "\t\t\t\tUFdeZWxzeoGIj5adpKuyucDHztXc4+rx+P8GDRQbIikw\n"
    "\t\t\t\tNz5FTFNaYWhvdn2Ei5KZoKeutbzDytHY3+bt9PsCCRAX\n"
    "\t\t\t\tHiUsMzpBSE9WXWRrcnmAh46VnKOqsbi/xs3U2+Lp8Pf+\n"
    "\t\t\t\tBQwTGiEoLzY9REtSWWBnbnV8g4qRmJ+mrbS7wsnQ197l\n"
    "\t\t\t\t7PP6AQgPFh0kKzI5QEdOVVxjanF4f4aNlJuiqbC3vsXM\n"
    "\t\t\t\t09rh6O/2/QQLEhkgJy41PENKUVhfZm10e4KJkJeepayz\n"
    "\t\t\t\tusHIz9bd5Ovy+QAHDhUcIyoxOD9GTVRbYmlwd36FjJOa\n"
    "\t\t\t\toaivtr3Ey9LZ4Ofu9fwDChEYHyYtNDtCSVBXXmVsc3qB\n"
    "\t\t\t\tiI+WnaSrsrnAx87V3OPq8fj/Bg0UGyIpMDc+RUxTWmFo\n"
    "\t\t\t\tb3Z9hIuSmaCnrrW8w8rR2N/m7fT7AgkQFx4lLDM6QUhP\n"
    "\t\t\t\tVl1ka3J5gIeOlZyjqrG4v8bN1Nvi6fD3/gUMExohKC82\n"
    "\t\t\t\tPQ==\n"
    "\t\t\t\t</data>\n"
    "\t\t\t\t<key>ID</key>\n"
    "\t\t\t\t<string>2</string>\n"
    "\t\t\t\t<key>Name</key>\n"
    "\t\t\t\t<string>Apple_Free (Apple_Free : 3)</string>\n"
    "\t\t\t</dict>\n"
    "\t\t\t<dict>\n"
    "\t\t\t\t<key>Attributes</key>\n"
    "\t\t\t\t<string>0x0000</string>\n"
    "\t\t\t\t<key>Data</key>\n"
    "\t\t\t\t<data>\n"
    "\t\t\t\tVVxjanF4f4aNlJuiqbC3vsXM09rh6O/2/QQLEhkgJy41\n"
    "\t\t\t\tPENKUVhfZm10e4KJkJeepayzusHIz9bd5Ovy+QAHDhUc\n"
    "\t\t\t\t</data>\n"
    "\t\t\t\t<key>ID</key>\n"
    "\t\t\t\t<string>3</string>\n"
    "\t\t\t\t<key>Name</key>\n"
    "\t\t\t\t<string>Apple_Free (Apple_Free : 4)</string>\n"
    "\t\t\t</dict>\n"
    "\t\t\t<dict>\n"
    "\t\t\t\t<key>Attributes</key>\n"
    "\t\t\t\t<string>0x0050</string>\n"
    "\t\t\t\t<key>Data</key>\n"
    "\t\t\t\t<data>\n"
    "\t\t\t\tZm10e4KJkJeepayzusHIz9bd5Ovy+QAHDhUcIyoxOD9G\n"
    "\t\t\t\tTVRbYmlwd36FjJOaoaivtr3Ey9LZ4Ofu9fwDChEYHyYt\n"
    "\t\t\t\tNDtCSVBXXmVsc3qBiI+WnaSrsrnAx87V3OPq8fj/Bg0U\n"
    "\t\t\t\tGyIpMDc+RUxTWmFob3Z9hIuSmaCnrrW8w8rR2N/m7fT7\n"
    "\t\t\t\tAgkQFx4lLDM6QUhPVl1ka3J5gIeOlZyjqrG4v8bN1Nvi\n"
    "\t\t\t\t6fD3/gUMExohKC82PURLUllgZ251fIOKkZifpq20u8LJ\n"
    "\t\t\t\t0Nfe5ezz+gEIDxYdJCsyOUBHTlVcY2pxeH+GjZSboqmw\n"
    "\t\t\t\tt77FzNPa4ejv9v0ECxIZICcuNTxDSlFYX2ZtdHuCiZCX\n"
    "\t\t\t\tnqWss7rByM/W3eTr8vkABw4VHCMqMTg/Rk1UW2JpcHd+\n"
    "\t\t\t\thYyTmqGor7a9xMvS2eDn7vX8AwoRGB8mLTQ7QklQV15l\n"
    "\t\t\t\tbHN6gYiPlp2kq7K5wMfO1dzj6vH4/wYNFBsiKTA3PkVM\n"
    "\t\t\t\tU1phaG92fYSLkpmgp661vMPK0djf5u30+wIJEBceJSwz\n"
    "\t\t\t\tOkFIT1ZdZGtyeYCHjpWco6qxuL/GzdTb4unw9/4FDBMa\n"
    "\t\t\t\tISgvNj1ES1JZYGdudXyDipGYn6attLvCydDX3uXs8/oB\n"
    "\t\t\t\tCA8WHSQrMjlAR05VXGNqcXh/ho2Um6KpsLe+xczT2uHo\n"
    "\t\t\t\t7/b9BAsSGSAnLjU8Q0pRWF9mbXR7gomQl56lrLO6wcjP\n"
    "\t\t\t\t1t3k6/L5AAcOFRwjKjE4P0ZNVFtiaXB3foWMk5qhqK+2\n"
    "\t\t\t\tvcTL0tng5+71/AMKERgfJi00O0JJUFdeZWxzeoGIj5ad\n"
    "\t\t\t\tpKuyucDHztXc4+rx+P8GDRQbIikwNz5FTFNaYWhvdn2E\n"
    "\t\t\t\ti5KZoKeutbzDytHY3+bt9PsCCRAXHiUsMzpBSE9WXWRr\n"
    "\t\t\t\tcnmAh46VnKOqsbi/xs3U2+Lp8Pf+BQwTGiEoLzY9REtS\n"
    "\t\t\t\tWWBnbnV8g4qRmJ+mrbS7wsnQ197l7PP6AQgPFh0kKzI5\n"
    "\t\t\t\tQEdOVVxjanF4f4aNlJuiqbC3vsXM09rh6O/2/QQLEhkg\n"
    "\t\t\t\tJy41PENKUVhfZm10e4KJkJeepayzusHIz9bd5Ovy+QAH\n"
    "\t\t\t\tDhUcIyoxOD9GTVRbYmlwd36FjJOaoaivtr3Ey9LZ4Ofu\n"
    "\t\t\t\t9fwDChEYHyYtNDtCSVBXXmVsc3qBiI+WnaSrsrnAx87V\n"
    "\t\t\t\t3OPq8fj/Bg0UGyIpMDc+RUxTWmFob3Z9hIuSmaCnrrW8\n"
    "\t\t\t\tw8rR2N/m7fT7AgkQFx4lLDM6QUhPVl1ka3J5gIeOlZyj\n"
    "\t\t\t\tqrG4v8bN1Nvi6fD3/gUMExohKC82PURLUllgZ251fIOK\n"
    "\t\t\t\tkZifpq20u8LJ0Nfe5ezz+gEIDxYdJCsyOUBHTlVcY2px\n"
    "\t\t\t\teH+GjZSboqmwt77FzNPa4ejv9v0ECxIZICcuNTxDSlFY\n"
    "\t\t\t\tX2ZtdHuCiZCXnqWss7rByM/W3eTr8vkABw4VHCMqMTg/\n"
    "\t\t\t\tRk1UW2JpcHd+hYyTmqGor7a9xMvS2eDn7vX8AwoRGB8m\n"
    "\t\t\t\tLTQ7QklQV15lbHN6gYiPlp2kq7K5wMfO1dzj6vH4/wYN\n"
    "\t\t\t\tFBsiKTA3PkVMU1phaG92fYSLkpmgp661vMPK0djf5u30\n"
    "\t\t\t\t+wIJEBceJSwzOkFIT1ZdZGtyeYCHjpWco6qxuL/GzdTb\n"
    "\t\t\t\t4unw9/4FDBMaISgvNj1ES1JZYGdudXyDipGYn6attLvC\n"
    "\t\t\t\tydDX3uXs8/oBCA8WHSQrMjlAR05VXGNqcXh/ho2Um6Kp\n"
    "\t\t\t\tsLe+xczT2uHo7/b9BAsSGSAnLjU8Q0pRWF9mbXR7gomQ\n"
    "\t\t\t\tl56lrLO6wcjP1t3k6/L5AAcOFRwjKjE4P0ZNVFtiaXB3\n"
    "\t\t\t\tfoWMk5qhqK+2vcTL0tng5+71/AMKERgfJi00O0JJUFde\n"
    "\t\t\t\tZWxzeoGIj5adpKuyucDHztXc4+rx+P8GDRQbIikwNz5F\n"
    "\t\t\t\tTFNaYWhvdn2Ei5KZoKeutbzDytHY3+bt9PsCCRAXHiUs\n"
    "\t\t\t\tMzpBSE9WXWRrcnmAh46VnKOqsbi/xs3U2+Lp8Pf+BQwT\n"
    "\t\t\t\tGiEoLzY9REtSWWBnbnV8g4qRmJ+mrbS7wsnQ197l7PP6\n"
    "\t\t\t\tAQgPFh0kKzI5QEdOVVxjanF4f4aNlJuiqbC3vsXM09rh\n"
    "\t\t\t\t6O/2/QQLEhkgJy41PENKUVhfZm10e4KJkJeepayzusHI\n"
    "\t\t\t\tz9bd5Ovy+QAHDhUcIyoxOD9GTVRbYmlwd36FjJOaoaiv\n"
    "\t\t\t\ttr3Ey9LZ4Ofu9fwDChEYHyYtNDtCSVBXXmVsc3qBiI+W\n"
    "\t\t\t\tnaSrsrnAx87V3OPq8fj/Bg0UGyIpMDc+RUxTWmFob3Z9\n"
    "\t\t\t\thIuSmaCnrrW8w8rR2N/m7fT7AgkQFx4lLDM6QUhPVl1k\n"
    "\t\t\t\ta3J5gIeOlZyjqrG4v8bN1Nvi6fD3/gUMExohKC82PURL\n"
    "\t\t\t\tUllgZ251fIOKkZifpq20u8LJ0Nfe5ezz+gEIDxYdJCsy\n"
    "\t\t\t\tOUBHTlVcY2pxeH+GjZSboqmwt77FzNPa4ejv9v0ECxIZ\n"
    "\t\t\t\tICcuNTxDSlFYX2ZtdHuCiZCXnqWss7rByM/W3eTr8vkA\n"
    "\t\t\t\tBw4VHCMqMTg/Rk1UW2JpcHd+hYyTmqGor7a9xMvS2eDn\n"
    "\t\t\t\t7vX8AwoRGB8mLTQ7QklQV15lbHN6gYiPlp2kq7K5wMfO\n"
    "\t\t\t\t1dzj6vH4/wYNFBsiKTA3PkVMU1phaG92fYSLkpmgp661\n"
    "\t\t\t\tvMPK0djf5u30+wIJEBceJSwzOkFIT1ZdZGtyeYCHjpWc\n"
    "\t\t\t\to6qxuL/GzdTb4unw9/4FDBMaISgvNj1ES1JZYGdudXyD\n"
    "\t\t\t\tipGYn6attLvCydDX3uXs8/oBCA8WHSQrMjlAR05VXGNq\n"
    "\t\t\t\tcXh/ho2Um6KpsLe+xczT2uHo7/b9BAsSGSAnLjU8Q0pR\n"
    "\t\t\t\tWF9mbXR7gomQl56lrLO6wcjP1t3k6/L5AAcOFRwjKjE4\n"
    "\t\t\t\tP0ZNVFtiaXB3foWMk5qhqK+2vcTL0tng5+71/AMKERgf\n"
    "\t\t\t\tJi00O0JJUFdeZWxzeoGIj5adpKuyucDHztXc4+rx+P8G\n"
    "\t\t\t\tDRQbIikwNz5FTFNaYWhvdn2Ei5KZoKeutbzDytHY3+bt\n"
    "\t\t\t\t9PsCCRAXHiUsMzpBSE9WXWRrcnmAh46VnKOqsbi/xs3U\n"
    "\t\t\t\t2+Lp8Pf+BQwTGiEoLzY9REtSWWBnbnV8g4qRmJ+mrbS7\n"
    "\t\t\t\twsnQ197l7PP6AQgPFh0kKzI5QEdOVVxjanF4f4aNlJui\n"
    "\t\t\t\tqbC3vsXM09rh6O/2/QQLEhkgJy41PENKUVhfZm10e4KJ\n"
    "\t\t\t\tkJeepayzusHIz9bd5Ovy+QAHDhUcIyoxOD9GTVRbYmlw\n"
    "\t\t\t\td36FjJOaoaivtr3Ey9LZ4Ofu9fwDChEYHyYtNDtCSVBX\n"
    "\t\t\t\tXmVsc3qBiI+WnaSrsrnAx87V3OPq8fj/Bg0UGyIpMDc+\n"
    "\t\t\t\tRUxTWmFob3Z9hIuSmaCnrrW8w8rR2N/m7fT7AgkQFx4l\n"
    "\t\t\t\tLDM6QUhPVl1ka3J5gIeOlZyjqrG4v8bN1Nvi6fD3/gUM\n"
    "\t\t\t\tExohKC82PURLUllgZ251fIOKkZifpq20u8LJ0Nfe5ezz\n"
    "\t\t\t\t+gEIDxYdJCsyOUBHTlVcY2pxeH+GjZSboqmwt77FzNPa\n"
    "\t\t\t\t4ejv9v0ECxIZICcuNTxDSlFYX2ZtdHuCiZCXnqWss7rB\n"
    "\t\t\t\tyM/W3eTr8vkABw4VHCMqMTg/Rk1UW2JpcHd+hYyTmqGo\n"
    "\t\t\t\tr7a9xMvS2eDn7vX8AwoRGB8mLTQ7QklQV15lbHN6gYiP\n"
    "\t\t\t\tlp2kq7K5wMfO1dzj6vH4/wYNFBsiKTA3PkVMU1phaG92\n"
    "\t\t\t\tfYSLkpmgp661vMPK0djf5u30+wIJEBceJSwzOkFIT1Zd\n"
    "\t\t\t\tZGtyeYCHjpWco6qxuL/GzdTb4unw9/4FDBMaISgvNj1E\n"
    "\t\t\t\tS1JZYGdudXyDipGYn6attLvCydDX3uXs8/oBCA8WHSQr\n"
    "\t\t\t\tMjlAR05VXGNqcXh/ho2Um6KpsLe+xczT2uHo7/b9BAsS\n"
    "\t\t\t\tGSAnLjU8Q0pRWF9mbXR7gomQl56lrLO6wcjP1t3k6/L5\n"
    "\t\t\t\tAAcOFRwjKjE4P0ZNVFtiaXB3foWMk5qhqK+2vcTL0tng\n"
    "\t\t\t\t5+71/AMKERgfJi00O0JJUFdeZWxzeoGIj5adpKuyucDH\n"
    "\t\t\t\tztXc4+rx+P8GDRQbIikwNz5FTFNaYWhvdn2Ei5KZoKeu\n"
    "\t\t\t\ttbzDytHY3+bt9PsCCRAXHiUsMzpBSE9WXWRrcnmAh46V\n"
    "\t\t\t\tnKOqsbi/xs3U2+Lp8Pf+BQwTGiEoLzY9REtSWWBnbnV8\n"
    "\t\t\t\tg4qRmJ+mrbS7wsnQ197l7PP6AQgPFh0kKzI5QEdOVVxj\n"
    "\t\t\t\tanF4f4aNlJuiqbC3vsXM09rh6O/2/QQLEhkgJy41PENK\n"
    "\t\t\t\tUVhfZm10e4KJkJeepayzusHIz9bd5Ovy+QAHDhUcIyox\n"
    "\t\t\t\tOD9GTVRbYmlwd36FjJOaoaivtr3Ey9LZ4Ofu9fwDChEY\n"
    "\t\t\t\tHyYtNDtCSVBXXmVsc3qBiI+WnaSrsrnAx87V3OPq8fj/\n"
    "\t\t\t\tBg0UGyIpMDc+RUxTWmFob3Z9hIuSmaCnrrW8w8rR2N/m\n"
    "\t\t\t\t7fT7AgkQFx4lLDM6QUhPVl1ka3J5gIeOlZyjqrG4v8bN\n"
    "\t\t\t\t1Nvi6fD3/gUMExohKC82PURLUllgZ251fIOKkZifpq20\n"
    "\t\t\t\tu8LJ0Nfe5ezz+gEIDxYdJCsyOUBHTlVcY2pxeH+GjZSb\n"
    "\t\t\t\t</data>\n"
    "\t\t\t\t<key>ID</key>\n"
    "\t\t\t\t<string>4</string>\n"
    "\t\t\t\t<key>Name</key>\n"
    "\t\t\t\t<string>disk image (Apple_HFSX : 5)</string>\n"
    "\t\t\t</dict>\n"
    "\t\t\t<dict>\n"
    "\t\t\t\t<key>Attributes</key>\n"
    "\t\t\t\t<string>0x0000</string>\n"
    "\t\t\t\t<key>Data</key>\n"
    "\t\t\t\t<data>\n"
    "\t\t\t\td36FjJOaoaivtr3Ey9LZ4Ofu9fwDChEYHyYtNDtCSVBX\n"
    "\t\t\t\tXmVsc3qBiI+WnaSrsrnAx87V3OPq8fj/Bg0UGyIpMDc+\n"
    "\t\t\t\tRUxTWmFob3Z9hIuSmaCnrrW8w8rR2N/m7fT7AgkQFx4l\n"
    "\t\t\t\tLDM6QUhPVl1ka3J5gIeOlZyjqrG4v8bN1Nvi6fD3/gUM\n"
    "\t\t\t\tExohKC82PURLUllgZ251fIOKkZifpq20u8LJ0Nfe5ezz\n"
    "\t\t\t\t+gEIDxYdJCsyOUBHTlVcY2pxeH+GjZSboqmwt77FzNPa\n"
    "\t\t\t\t4ejv9v0ECxIZICcuNTxDSlFYX2ZtdHuCiZCXnqWss7rB\n"
    "\t\t\t\tyM/W3eTr8vkABw4VHCMqMTg/Rk1UW2JpcHd+hYyTmqGo\n"
    "\t\t\t\tr7a9xMvS2eDn7vX8AwoRGB8mLTQ7QklQV15lbHN6gYiP\n"
    "\t\t\t\t</data>\n"
    "\t\t\t\t<key>ID</key>\n"
    "\t\t\t\t<string>5</string>\n"
    "\t\t\t\t<key>Name</key>\n"
    "\t\t\t\t<string>Apple_Free (Apple_Free : 6)</string>\n"
    "\t\t\t</dict>\n"
    "\t\t</array>\n"
    "\t</dict>\n"
    "</dict>\n"
    "</plist>\n";

/* The same base64 wrapped at 64 characters instead, to pin down that line
 * geometry is not something the decoder may depend on. */
static const char k_wide_data[] =
    "<plist version=\"1.0\">\n"
    "<dict>\n"
    "\t<key>Blob</key>\n"
    "\t<data>\n"
    "\tQUJDREVGR0hJSktMTU5PUFFSU1RVVldYWVphYmNkZWZnaGlqa2xtbm9wcXJzdHV2\n"
    "\td3h5ejAxMjM0NTY3ODk=\n"
    "\t</data>\n"
    "</dict>\n"
    "</plist>\n";

static const char k_wide_payload[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789";

#define FORK_ENTRIES 7u

static const size_t k_fork_sizes[FORK_ENTRIES] = {
    40u, 132u, 33u, 1024u, 66u, 3300u, 297u
};
static const char *const k_fork_names[FORK_ENTRIES] = {
    "Driver Descriptor Map (DDM : 0)",
    "Apple (Apple_partition_map : 1)",
    "Macintosh (Apple_Driver_ATAPI : 2)",
    "Apple_Free (Apple_Free : 3)",
    "Apple_Free (Apple_Free : 4)",
    "disk image (Apple_HFSX : 5)",
    "Apple_Free (Apple_Free : 6)"
};
static const char *const k_fork_ids[FORK_ENTRIES] = {
    "-1", "0", "1", "2", "3", "4", "5"
};
static const char *const k_fork_attrs[FORK_ENTRIES] = {
    "0x0050", "0x0000", "0x0000", "0x0000", "0x0000", "0x0050", "0x0000"
};

static void fork_blob(unsigned k, uint8_t *out, size_t n) {
    for (size_t i = 0; i < n; i++)
        out[i] = (uint8_t)(0x11u * (k + 1u) + 7u * (unsigned)i);
}

/* ------------------------------------------------------------------------ */
/* Helpers                                                                   */
/* ------------------------------------------------------------------------ */

#define DOC(lit) (const uint8_t *)(lit), (sizeof (lit) - 1u)

static void expect_st(vmfw_test_t *t, vmfw_plist_status_t got,
                      vmfw_plist_status_t want, const char *what) {
    VMFW_T_CHECK(t, got == want, "%s: got \"%s\" want \"%s\"", what,
                 vmfw_plist_strerror(got), vmfw_plist_strerror(want));
}

static void expect_str(vmfw_test_t *t, const vmfw_plist_t *pl,
                       const char *path, const char *want) {
    char buf[256];
    const vmfw_plist_status_t st =
        vmfw_plist_get_string(pl, path, buf, sizeof buf);
    if (st != VMFW_PLIST_OK) {
        VMFW_T_CHECK(t, 0, "%s: %s", path, vmfw_plist_strerror(st));
        return;
    }
    VMFW_T_EQ_STR(t, buf, want, path);
}

static void expect_str_st(vmfw_test_t *t, const vmfw_plist_t *pl,
                          const char *path, vmfw_plist_status_t want) {
    char buf[256];
    expect_st(t, vmfw_plist_get_string(pl, path, buf, sizeof buf), want, path);
}

static void expect_count(vmfw_test_t *t, const vmfw_plist_t *pl,
                         const char *path, size_t want) {
    size_t n = 0;
    const vmfw_plist_status_t st = vmfw_plist_array_count(pl, path, &n);
    if (st != VMFW_PLIST_OK) {
        VMFW_T_CHECK(t, 0, "%s: %s", path, vmfw_plist_strerror(st));
        return;
    }
    VMFW_T_EQ_U(t, n, want, path);
}

/* Open a document that is expected to be a plist, and say so if it is not. */
static int open_doc(vmfw_test_t *t, vmfw_plist_t *pl,
                    const void *xml, size_t len, const char *what) {
    const vmfw_plist_status_t st =
        vmfw_plist_init(pl, (const uint8_t *)xml, len);
    VMFW_T_CHECK(t, st == VMFW_PLIST_OK, "%s: init -> %s", what,
                 vmfw_plist_strerror(st));
    return st == VMFW_PLIST_OK;
}

static int app(char *buf, size_t cap, size_t *o, const char *s) {
    size_t n = 0;
    while (s[n]) n++;
    if (*o + n + 1u > cap) return 0;
    memcpy(buf + *o, s, n);
    *o += n;
    buf[*o] = '\0';
    return 1;
}

/* n nested <array>s wrapping `inner`, so the deepest element sits at document
 * level n + 1 (the <plist> element itself is level 1). */
static size_t build_nested(char *buf, size_t cap, unsigned n,
                           const char *inner) {
    size_t o = 0;
    if (!app(buf, cap, &o, "<plist version=\"1.0\">")) return 0;
    for (unsigned i = 0; i < n; i++)
        if (!app(buf, cap, &o, "<array>")) return 0;
    if (!app(buf, cap, &o, inner)) return 0;
    for (unsigned i = 0; i < n; i++)
        if (!app(buf, cap, &o, "</array>")) return 0;
    if (!app(buf, cap, &o, "</plist>")) return 0;
    return o;
}

/* "0/0/0/..." with `n` components; n == 0 gives the empty path. */
static void build_zero_path(char *buf, size_t cap, unsigned n) {
    size_t o = 0;
    buf[0] = '\0';
    for (unsigned i = 0; i < n; i++)
        if (!app(buf, cap, &o, i ? "/0" : "0")) return;
}

/* ------------------------------------------------------------------------ */
/* Sections                                                                  */
/* ------------------------------------------------------------------------ */

static void test_init(vmfw_test_t *t) {
    VMFW_T_SECTION(t, "init");

    vmfw_plist_t pl;

    expect_st(t, vmfw_plist_init(NULL, (const uint8_t *)k_restore, 10u),
              VMFW_PLIST_ERR_INVALID_ARGUMENT, "init(NULL)");
    expect_st(t, vmfw_plist_init(&pl, NULL, 0u),
              VMFW_PLIST_ERR_INVALID_ARGUMENT, "init(xml=NULL)");

    expect_st(t, vmfw_plist_init(&pl, DOC("")),
              VMFW_PLIST_ERR_NOT_XML, "init(empty)");
    expect_st(t, vmfw_plist_init(&pl, DOC("hello, world")),
              VMFW_PLIST_ERR_NOT_XML, "init(text)");
    expect_st(t, vmfw_plist_init(&pl, DOC("<?xml version=\"1.0\"?><dict/>")),
              VMFW_PLIST_ERR_NOT_XML, "init(no <plist>)");
    /* The name must match exactly, or "<plistoid>" would open a document. */
    expect_st(t, vmfw_plist_init(&pl, DOC("<plistoid/>")),
              VMFW_PLIST_ERR_NOT_XML, "init(<plistoid>)");
    expect_st(t, vmfw_plist_init(&pl, DOC("</plist>")),
              VMFW_PLIST_ERR_NOT_XML, "init(end tag only)");
    /* A commented-out root is not a root. Searching the raw bytes for "<plist"
     * would get this wrong. */
    expect_st(t, vmfw_plist_init(&pl, DOC("<!-- <plist version=\"1.0\"> --><x/>")),
              VMFW_PLIST_ERR_NOT_XML, "init(<plist> in a comment)");

    expect_st(t, vmfw_plist_init(&pl, DOC("<!-- never closed")),
              VMFW_PLIST_ERR_MALFORMED, "init(unterminated comment)");
    expect_st(t, vmfw_plist_init(&pl, DOC("<?xml version=\"1.0\"")),
              VMFW_PLIST_ERR_MALFORMED, "init(unterminated declaration)");
    expect_st(t, vmfw_plist_init(&pl, DOC("<!DOCTYPE plist PUBLIC \"x\"")),
              VMFW_PLIST_ERR_MALFORMED, "init(unterminated doctype)");
    expect_st(t, vmfw_plist_init(&pl, DOC("<plist version=\"1.0\"")),
              VMFW_PLIST_ERR_MALFORMED, "init(unterminated tag)");
    expect_st(t, vmfw_plist_init(&pl, DOC("<")),
              VMFW_PLIST_ERR_MALFORMED, "init(lone <)");
    expect_st(t, vmfw_plist_init(&pl, DOC("<>")),
              VMFW_PLIST_ERR_MALFORMED, "init(nameless tag)");

    /* A DOCTYPE with an internal subset and a '>' inside a quoted identifier
     * must still be stepped over in one piece. */
    expect_st(t, vmfw_plist_init(&pl, DOC(
                  "<!DOCTYPE plist PUBLIC \"a>b\" [ <!ENTITY x \"y>z\"> ]>"
                  "<plist version=\"1.0\"><dict/></plist>")),
              VMFW_PLIST_OK, "init(doctype with subset)");

    /*
     * And it must be stepped over far enough. A scanner that stopped at the
     * first '>' regardless of quoting would resume inside the public
     * identifier, where a whole decoy document is hiding, and answer from that
     * instead of from the real root.
     */
    if (open_doc(t, &pl, DOC(
            "<!DOCTYPE plist PUBLIC \"x>y <plist version='1.0'><dict>"
            "<key>a</key><string>decoy</string></dict></plist>\" \"d.dtd\">"
            "<plist version=\"1.0\"><dict><key>a</key><string>real</string>"
            "</dict></plist>"), "doctype hiding a decoy root"))
        expect_str(t, &pl, "a", "real");

    /* The same rule inside a start tag: an attribute value may contain '>'. */
    if (open_doc(t, &pl, DOC("<plist version=\"1.0\"><dict><key>a</key>"
                             "<string x=\"p>q\">real</string></dict></plist>"),
                 "'>' inside an attribute"))
        expect_str(t, &pl, "a", "real");

    /* A tag cannot be an end tag and a self-closing tag at once. */
    if (open_doc(t, &pl, DOC("<plist version=\"1.0\"><dict><key>a</key>"
                             "<string>b</string></dict/></plist>"),
                 "</dict/>"))
        expect_str_st(t, &pl, "a", VMFW_PLIST_ERR_MALFORMED);

    /* An empty root is a valid plist that names nothing. */
    if (open_doc(t, &pl, DOC("<plist version=\"1.0\"/>"), "<plist/>")) {
        expect_str_st(t, &pl, "Anything", VMFW_PLIST_ERR_NOT_FOUND);
        expect_st(t, vmfw_plist_array_count(&pl, "Anything", &(size_t){0}),
                  VMFW_PLIST_ERR_NOT_FOUND, "<plist/> count");
    }
}

static void test_restore(vmfw_test_t *t) {
    VMFW_T_SECTION(t, "restore");

    vmfw_plist_t pl;
    if (!open_doc(t, &pl, k_restore, sizeof k_restore - 1u, "Restore.plist"))
        return;

    VMFW_T_EQ_U(t, sizeof k_restore - 1u, 1804u, "Restore.plist size");

    expect_str(t, &pl, "ProductType",         "iPhone1,2");
    expect_str(t, &pl, "ProductVersion",      "3.1.3");
    expect_str(t, &pl, "ProductBuildVersion", "7E18");
    expect_str(t, &pl, "DeviceClass",         "iPhone");
    expect_str(t, &pl, "FirmwareDirectory",   "Firmware");

    expect_str(t, &pl, "DeviceMap/0/BoardConfig", "n82ap");
    expect_str(t, &pl, "DeviceMap/0/Platform",    "s5l8900x");

    expect_str(t, &pl, "KernelCachesByPlatform/s5l8900x/Release",
               "kernelcache.release.s5l8900x");
    expect_str(t, &pl, "RestoreKernelCaches/Release",
               "kernelcache.release.s5l8900x");

    expect_str(t, &pl, "SystemRestoreImages/User", "018-6482-014.dmg");
    expect_str(t, &pl, "RestoreRamDisks/User",     "018-6494-014.dmg");
    expect_str(t, &pl, "RestoreRamDisks/Update",   "018-6488-015.dmg");
    expect_str(t, &pl, "RamDisksByPlatform/s5l8900x/User",   "018-6494-014.dmg");
    expect_str(t, &pl, "RamDisksByPlatform/s5l8900x/Update", "018-6488-015.dmg");

    expect_str(t, &pl, "SupportedProductTypes/0", "iPhone1,2");

    expect_count(t, &pl, "DeviceMap",                     1u);
    expect_count(t, &pl, "SupportedProductTypes",         1u);
    expect_count(t, &pl, "SupportedProductTypeIDs/DFU",      2u);
    expect_count(t, &pl, "SupportedProductTypeIDs/Recovery", 1u);
}

static void test_restore_errors(vmfw_test_t *t) {
    VMFW_T_SECTION(t, "restore/errors");

    vmfw_plist_t pl;
    if (!open_doc(t, &pl, k_restore, sizeof k_restore - 1u, "Restore.plist"))
        return;

    char buf[256];

    /* Nothing named that. */
    expect_str_st(t, &pl, "NoSuchKey",           VMFW_PLIST_ERR_NOT_FOUND);
    expect_str_st(t, &pl, "DeviceMap/1",         VMFW_PLIST_ERR_NOT_FOUND);
    expect_str_st(t, &pl, "DeviceMap/0/NoSuch",  VMFW_PLIST_ERR_NOT_FOUND);
    expect_str_st(t, &pl, "SupportedProductTypes/1", VMFW_PLIST_ERR_NOT_FOUND);
    /* A digit component only indexes an array; the root is a dict. */
    expect_str_st(t, &pl, "0",                   VMFW_PLIST_ERR_NOT_FOUND);
    /* Descending through a scalar names nothing. */
    expect_str_st(t, &pl, "ProductType/0",       VMFW_PLIST_ERR_NOT_FOUND);
    expect_str_st(t, &pl, "ProductType/x",       VMFW_PLIST_ERR_NOT_FOUND);
    /* An index too large for size_t saturates instead of wrapping. */
    expect_str_st(t, &pl, "DeviceMap/99999999999999999999999999",
                  VMFW_PLIST_ERR_NOT_FOUND);

    /*
     * A component matches only at the current level. Every one of these names
     * a key that exists somewhere in the document, nested. If the walk ever
     * started searching downwards, "Platform" would resolve to "s5l8900x" and
     * an importer would silently accept a manifest for the wrong device.
     */
    expect_str_st(t, &pl, "BoardConfig", VMFW_PLIST_ERR_NOT_FOUND);
    expect_str_st(t, &pl, "Platform",    VMFW_PLIST_ERR_NOT_FOUND);
    expect_str_st(t, &pl, "Release",     VMFW_PLIST_ERR_NOT_FOUND);
    expect_str_st(t, &pl, "User",        VMFW_PLIST_ERR_NOT_FOUND);
    expect_str_st(t, &pl, "s5l8900x",    VMFW_PLIST_ERR_NOT_FOUND);
    expect_str_st(t, &pl, "DFU",         VMFW_PLIST_ERR_NOT_FOUND);

    /* Found, but not that type. */
    expect_str_st(t, &pl, "DeviceMap",         VMFW_PLIST_ERR_WRONG_TYPE);
    expect_str_st(t, &pl, "DeviceMap/0",       VMFW_PLIST_ERR_WRONG_TYPE);
    expect_str_st(t, &pl, "DeviceMap/0/BDID",  VMFW_PLIST_ERR_WRONG_TYPE);
    expect_str_st(t, &pl, "",                  VMFW_PLIST_ERR_WRONG_TYPE);
    expect_st(t, vmfw_plist_array_count(&pl, "ProductType", &(size_t){0}),
              VMFW_PLIST_ERR_WRONG_TYPE, "count(ProductType)");
    expect_st(t, vmfw_plist_array_count(&pl, "", &(size_t){0}),
              VMFW_PLIST_ERR_WRONG_TYPE, "count(root dict)");
    {
        const uint8_t *span = NULL;
        size_t span_len = 0;
        expect_st(t, vmfw_plist_get_data_span(&pl, "ProductType",
                                              &span, &span_len),
                  VMFW_PLIST_ERR_WRONG_TYPE, "data(ProductType)");
        VMFW_T_EQ_U(t, span_len, 0u, "refused span length");
    }

    /*
     * Truncation is refused, never silent. "iPhone1,2" is nine characters, so
     * ten bytes is exactly enough and nine is one short.
     */
    buf[0] = 'x';
    expect_st(t, vmfw_plist_get_string(&pl, "ProductType", buf, 9u),
              VMFW_PLIST_ERR_TOO_LONG, "get_string(cap 9)");
    VMFW_T_CHECK(t, buf[0] == '\0', "refused get_string left \"%s\" behind", buf);
    expect_st(t, vmfw_plist_get_string(&pl, "ProductType", buf, 10u),
              VMFW_PLIST_OK, "get_string(cap 10)");
    VMFW_T_EQ_STR(t, buf, "iPhone1,2", "get_string(cap 10) value");

    /* Caller bugs are told apart from missing keys. */
    expect_st(t, vmfw_plist_get_string(&pl, "ProductType", NULL, 16u),
              VMFW_PLIST_ERR_INVALID_ARGUMENT, "get_string(out=NULL)");
    expect_st(t, vmfw_plist_get_string(&pl, "ProductType", buf, 0u),
              VMFW_PLIST_ERR_INVALID_ARGUMENT, "get_string(cap 0)");
    expect_st(t, vmfw_plist_get_string(&pl, NULL, buf, sizeof buf),
              VMFW_PLIST_ERR_INVALID_ARGUMENT, "get_string(path=NULL)");
    expect_str_st(t, &pl, "/ProductType",  VMFW_PLIST_ERR_INVALID_ARGUMENT);
    expect_str_st(t, &pl, "ProductType/",  VMFW_PLIST_ERR_INVALID_ARGUMENT);
    expect_str_st(t, &pl, "DeviceMap//0",  VMFW_PLIST_ERR_INVALID_ARGUMENT);
    expect_st(t, vmfw_plist_array_count(&pl, "DeviceMap", NULL),
              VMFW_PLIST_ERR_INVALID_ARGUMENT, "count(out=NULL)");
    {
        size_t span_len = 0;
        expect_st(t, vmfw_plist_get_data_span(&pl, "x", NULL, &span_len),
                  VMFW_PLIST_ERR_INVALID_ARGUMENT, "data(out=NULL)");
    }

    /* An uninitialised view is refused rather than dereferenced. */
    {
        vmfw_plist_t blank = { NULL, 0u };
        expect_st(t, vmfw_plist_get_string(&blank, "ProductType",
                                           buf, sizeof buf),
                  VMFW_PLIST_ERR_INVALID_ARGUMENT, "get_string(blank view)");
    }
}

static void test_fork(vmfw_test_t *t) {
    VMFW_T_SECTION(t, "fork");

    vmfw_plist_t pl;
    if (!open_doc(t, &pl, k_fork, sizeof k_fork - 1u, "resource fork"))
        return;

    expect_count(t, &pl, "resource-fork/blkx", FORK_ENTRIES);

    char path[64];
    for (unsigned k = 0; k < FORK_ENTRIES; k++) {
        size_t o = 0;
        char idx[8];
        idx[0] = (char)('0' + k);
        idx[1] = '\0';

        path[0] = '\0';
        (void)app(path, sizeof path, &o, "resource-fork/blkx/");
        (void)app(path, sizeof path, &o, idx);
        const size_t base = o;

        (void)app(path, sizeof path, &o, "/Name");
        expect_str(t, &pl, path, k_fork_names[k]);

        o = base; path[o] = '\0';
        (void)app(path, sizeof path, &o, "/ID");
        expect_str(t, &pl, path, k_fork_ids[k]);

        o = base; path[o] = '\0';
        (void)app(path, sizeof path, &o, "/Attributes");
        expect_str(t, &pl, path, k_fork_attrs[k]);
    }

    /* The paths this project actually uses. */
    expect_str(t, &pl, "resource-fork/blkx/5/Name", "disk image (Apple_HFSX : 5)");
    expect_str(t, &pl, "resource-fork/blkx/0/ID",   "-1");

    /* One past the end, and the nesting rule again. */
    expect_str_st(t, &pl, "resource-fork/blkx/7/Name", VMFW_PLIST_ERR_NOT_FOUND);
    expect_str_st(t, &pl, "blkx",                      VMFW_PLIST_ERR_NOT_FOUND);
    expect_str_st(t, &pl, "Name",                      VMFW_PLIST_ERR_NOT_FOUND);
    expect_str_st(t, &pl, "resource-fork/blkx/5/Data", VMFW_PLIST_ERR_WRONG_TYPE);
    expect_st(t, vmfw_plist_array_count(&pl, "resource-fork", &(size_t){0}),
              VMFW_PLIST_ERR_WRONG_TYPE, "count(resource-fork)");
}

static void test_fork_data(vmfw_test_t *t) {
    VMFW_T_SECTION(t, "fork/data");

    vmfw_plist_t pl;
    if (!open_doc(t, &pl, k_fork, sizeof k_fork - 1u, "resource fork"))
        return;

    static uint8_t decoded[4096];
    static uint8_t expect[4096];

    char path[64];
    for (unsigned k = 0; k < FORK_ENTRIES; k++) {
        size_t o = 0;
        char idx[2];
        idx[0] = (char)('0' + k);
        idx[1] = '\0';
        path[0] = '\0';
        (void)app(path, sizeof path, &o, "resource-fork/blkx/");
        (void)app(path, sizeof path, &o, idx);
        (void)app(path, sizeof path, &o, "/Data");

        const uint8_t *span = NULL;
        size_t span_len = 0;
        const vmfw_plist_status_t st =
            vmfw_plist_get_data_span(&pl, path, &span, &span_len);
        if (st != VMFW_PLIST_OK) {
            VMFW_T_CHECK(t, 0, "%s: %s", path, vmfw_plist_strerror(st));
            continue;
        }

        /* The span points into the caller's bytes; nothing was copied. */
        VMFW_T_CHECK(t, span >= (const uint8_t *)k_fork &&
                        span + span_len <=
                            (const uint8_t *)k_fork + sizeof k_fork - 1u,
                     "%s: span escapes the document", path);
        /* Trimmed: the base64 body begins and ends on alphabet characters. */
        VMFW_T_CHECK(t, span_len > 0u && span[0] != '\n' && span[0] != '\t' &&
                        span[span_len - 1u] != '\n' &&
                        span[span_len - 1u] != '\t',
                     "%s: span not trimmed", path);

        const size_t sized = vmfw_base64_decoded_size(span, span_len);
        VMFW_T_EQ_U(t, sized, k_fork_sizes[k], "decoded_size");

        size_t got = 0;
        expect_st(t, vmfw_base64_decode(span, span_len, decoded,
                                        sizeof decoded, &got),
                  VMFW_PLIST_OK, "decode");
        VMFW_T_EQ_U(t, got, k_fork_sizes[k], "decoded length");
        if (got == k_fork_sizes[k]) {
            fork_blob(k, expect, got);
            VMFW_T_EQ_MEM(t, decoded, expect, got, "decoded bytes");
        }

        /* A buffer one byte short is refused, not filled. */
        expect_st(t, vmfw_base64_decode(span, span_len, decoded,
                                        k_fork_sizes[k] - 1u, &got),
                  VMFW_PLIST_ERR_TOO_LONG, "decode into a short buffer");
        VMFW_T_EQ_U(t, got, 0u, "short decode reported no bytes");
    }

    /* The same content wrapped at 64 characters instead of Apple's 44 must
     * decode to the same bytes: line geometry is not part of the format. */
    vmfw_plist_t wide;
    if (open_doc(t, &wide, DOC(k_wide_data), "64-column data")) {
        const uint8_t *span = NULL;
        size_t span_len = 0;
        expect_st(t, vmfw_plist_get_data_span(&wide, "Blob", &span, &span_len),
                  VMFW_PLIST_OK, "wide data span");
        size_t got = 0;
        expect_st(t, vmfw_base64_decode(span, span_len, decoded,
                                        sizeof decoded, &got),
                  VMFW_PLIST_OK, "wide data decode");
        VMFW_T_EQ_U(t, got, sizeof k_wide_payload - 1u, "wide data length");
        VMFW_T_EQ_U(t, vmfw_base64_decoded_size(span, span_len),
                    sizeof k_wide_payload - 1u, "wide decoded_size");
        if (got == sizeof k_wide_payload - 1u)
            VMFW_T_EQ_MEM(t, decoded, k_wide_payload, got, "wide data bytes");
    }
}

static void test_structure(vmfw_test_t *t) {
    VMFW_T_SECTION(t, "structure");

    vmfw_plist_t pl;

    /* A <key> with nothing after it. Guessing that the dict simply ends would
     * hide a truncated manifest. */
    if (open_doc(t, &pl, DOC("<plist version=\"1.0\"><dict>"
                             "<key>a</key></dict></plist>"), "key/no value"))
        expect_str_st(t, &pl, "a", VMFW_PLIST_ERR_MALFORMED);

    /* Two keys in a row: the entries do not pair up. */
    if (open_doc(t, &pl, DOC("<plist version=\"1.0\"><dict>"
                             "<key>a</key><key>b</key><string>c</string>"
                             "</dict></plist>"), "key/key"))
        expect_str_st(t, &pl, "a", VMFW_PLIST_ERR_MALFORMED);

    /* A value with no key. */
    if (open_doc(t, &pl, DOC("<plist version=\"1.0\"><dict>"
                             "<string>c</string></dict></plist>"), "value/no key"))
        expect_str_st(t, &pl, "a", VMFW_PLIST_ERR_MALFORMED);

    /* An unbalanced </dict> after the root object. Reading the <plist> element
     * as a whole is what makes this visible instead of ignored. */
    if (open_doc(t, &pl, DOC("<plist version=\"1.0\"><dict>"
                             "<key>a</key><string>b</string></dict></dict>"
                             "</plist>"), "extra </dict>"))
        expect_str_st(t, &pl, "a", VMFW_PLIST_ERR_MALFORMED);

    /* Mis-nested containers. A depth counter would call this balanced. */
    if (open_doc(t, &pl, DOC("<plist version=\"1.0\"><dict><array>"
                             "</dict></array></plist>"), "mis-nested"))
        expect_str_st(t, &pl, "a", VMFW_PLIST_ERR_MALFORMED);

    /* A container that is never closed. */
    if (open_doc(t, &pl, DOC("<plist version=\"1.0\"><dict>"
                             "<key>a</key><string>b</string>"), "unclosed"))
        expect_str_st(t, &pl, "a", VMFW_PLIST_ERR_MALFORMED);

    /* A tag that is never closed. */
    if (open_doc(t, &pl, DOC("<plist version=\"1.0\"><dict"), "unclosed tag"))
        expect_str_st(t, &pl, "a", VMFW_PLIST_ERR_MALFORMED);

    /* An end tag with junk in it is not an end tag. */
    if (open_doc(t, &pl, DOC("<plist version=\"1.0\"><dict>"
                             "<key>a</key><string>b</string></dict junk>"
                             "</plist>"), "junk in end tag"))
        expect_str_st(t, &pl, "a", VMFW_PLIST_ERR_MALFORMED);

    /* CDATA is refused rather than half-supported. */
    if (open_doc(t, &pl, DOC("<plist version=\"1.0\"><dict><key>a</key>"
                             "<string><![CDATA[b]]></string></dict></plist>"),
                 "cdata"))
        expect_str_st(t, &pl, "a", VMFW_PLIST_ERR_MALFORMED);

    /* Markup inside what should be character data. */
    if (open_doc(t, &pl, DOC("<plist version=\"1.0\"><dict><key>a</key>"
                             "<string><b>x</b></string></dict></plist>"),
                 "markup in string"))
        expect_str_st(t, &pl, "a", VMFW_PLIST_ERR_MALFORMED);

    /* Comments between entries are skipped, not treated as content. */
    if (open_doc(t, &pl, DOC("<plist version=\"1.0\"><dict>"
                             "<!-- <key>a</key><string>wrong</string> -->"
                             "<key>a</key><!-- x --><string>right</string>"
                             "</dict></plist>"), "comments"))
        expect_str(t, &pl, "a", "right");

    /* Content after </plist> is not part of the document. */
    if (open_doc(t, &pl, DOC("<plist version=\"1.0\"><dict><key>a</key>"
                             "<string>b</string></dict></plist>\n<!-- end -->\n"),
                 "trailing"))
        expect_str(t, &pl, "a", "b");
}

static void test_self_closing(vmfw_test_t *t) {
    VMFW_T_SECTION(t, "self-closing");

    vmfw_plist_t pl;

    /*
     * A self-closing container must not be descended into and must not
     * unbalance the walk. If <dict/> were treated as an opening tag, "b" would
     * be swallowed and this document would look truncated.
     */
    if (open_doc(t, &pl, DOC("<plist version=\"1.0\"><dict>"
                             "<key>a</key><dict/>"
                             "<key>b</key><string>B</string>"
                             "<key>c</key><array/>"
                             "<key>d</key><string>D</string>"
                             "</dict></plist>"), "self-closing containers")) {
        expect_str(t, &pl, "b", "B");
        expect_str(t, &pl, "d", "D");
        expect_str_st(t, &pl, "a",   VMFW_PLIST_ERR_WRONG_TYPE);
        expect_str_st(t, &pl, "a/x", VMFW_PLIST_ERR_NOT_FOUND);
        expect_str_st(t, &pl, "c/0", VMFW_PLIST_ERR_NOT_FOUND);
        expect_count(t, &pl, "c", 0u);
        expect_st(t, vmfw_plist_array_count(&pl, "a", &(size_t){0}),
                  VMFW_PLIST_ERR_WRONG_TYPE, "count(<dict/>)");
    }

    /* Every self-closing form counts as exactly one array element. */
    if (open_doc(t, &pl, DOC("<plist version=\"1.0\"><array>"
                             "<dict/><array/><true/><false/>"
                             "<string/><data/></array></plist>"),
                 "self-closing elements")) {
        expect_count(t, &pl, "", 6u);
        expect_str(t, &pl, "4", "");                 /* <string/> */
        expect_str_st(t, &pl, "2", VMFW_PLIST_ERR_WRONG_TYPE);  /* <true/> */
        expect_str_st(t, &pl, "3", VMFW_PLIST_ERR_WRONG_TYPE);  /* <false/> */
        const uint8_t *span = NULL;
        size_t span_len = 1u;
        expect_st(t, vmfw_plist_get_data_span(&pl, "5", &span, &span_len),
                  VMFW_PLIST_OK, "span of <data/>");
        VMFW_T_EQ_U(t, span_len, 0u, "<data/> span length");
        size_t got = 1u;
        expect_st(t, vmfw_base64_decode(span, span_len, NULL, 0u, &got),
                  VMFW_PLIST_OK, "decode of <data/>");
        VMFW_T_EQ_U(t, got, 0u, "<data/> decoded length");
    }

    /* An explicitly empty string is the empty string, not a missing one. */
    if (open_doc(t, &pl, DOC("<plist version=\"1.0\"><dict>"
                             "<key>a</key><string></string>"
                             "<key>b</key><string/></dict></plist>"),
                 "empty strings")) {
        expect_str(t, &pl, "a", "");
        expect_str(t, &pl, "b", "");
        /* An empty value still needs room for its terminator. */
        expect_st(t, vmfw_plist_get_string(&pl, "a", (char[1]){'x'}, 1u),
                  VMFW_PLIST_OK, "empty string into a 1-byte buffer");
    }

    /* An empty <data> with only Apple's indentation inside it. */
    if (open_doc(t, &pl, DOC("<plist version=\"1.0\"><dict>\n"
                             "\t<key>a</key>\n\t<data>\n\t</data>\n"
                             "</dict>\n</plist>\n"), "empty data")) {
        const uint8_t *span = NULL;
        size_t span_len = 1u;
        expect_st(t, vmfw_plist_get_data_span(&pl, "a", &span, &span_len),
                  VMFW_PLIST_OK, "empty data span");
        VMFW_T_EQ_U(t, span_len, 0u, "empty data span length");
        VMFW_T_EQ_U(t, vmfw_base64_decoded_size(span, span_len), 0u,
                    "empty data decoded_size");
    }
}

static void test_depth(vmfw_test_t *t) {
    VMFW_T_SECTION(t, "depth");

    /*
     * The <plist> element is level 1, so the deepest legal element sits at
     * VMFW_PLIST_MAX_DEPTH. That makes 31 bare nested arrays the limit, and 30
     * once a <string> has to fit inside them. Both edges are pinned here: a cap
     * that is off by one is a cap that either refuses real documents or does
     * not bound anything.
     */
    static char doc[1024];
    char path[128];
    vmfw_plist_t pl;

    size_t len = build_nested(doc, sizeof doc, VMFW_PLIST_MAX_DEPTH - 1u, "");
    VMFW_T_CHECK(t, len != 0u, "nested document did not fit");
    if (open_doc(t, &pl, doc, len, "31 nested arrays")) {
        build_zero_path(path, sizeof path, VMFW_PLIST_MAX_DEPTH - 2u);
        expect_count(t, &pl, path, 0u);
    }

    len = build_nested(doc, sizeof doc, VMFW_PLIST_MAX_DEPTH, "");
    VMFW_T_CHECK(t, len != 0u, "nested document did not fit");
    if (open_doc(t, &pl, doc, len, "32 nested arrays"))
        expect_st(t, vmfw_plist_array_count(&pl, "", &(size_t){0}),
                  VMFW_PLIST_ERR_TOO_DEEP, "count(32 nested)");

    len = build_nested(doc, sizeof doc, VMFW_PLIST_MAX_DEPTH - 2u,
                       "<string>deep</string>");
    VMFW_T_CHECK(t, len != 0u, "nested document did not fit");
    if (open_doc(t, &pl, doc, len, "30 nested arrays + string")) {
        build_zero_path(path, sizeof path, VMFW_PLIST_MAX_DEPTH - 2u);
        expect_str(t, &pl, path, "deep");
    }

    len = build_nested(doc, sizeof doc, VMFW_PLIST_MAX_DEPTH - 1u,
                       "<string>deep</string>");
    VMFW_T_CHECK(t, len != 0u, "nested document did not fit");
    if (open_doc(t, &pl, doc, len, "31 nested arrays + string")) {
        build_zero_path(path, sizeof path, VMFW_PLIST_MAX_DEPTH - 1u);
        expect_str_st(t, &pl, path, VMFW_PLIST_ERR_TOO_DEEP);
        /* The refusal does not depend on asking for the deep part: the whole
         * document is walked, so a shallow lookup is refused too. */
        expect_str_st(t, &pl, "nope", VMFW_PLIST_ERR_TOO_DEEP);
    }

    /* Far past the cap, and cheap to refuse. */
    len = build_nested(doc, sizeof doc, 64u, "");
    VMFW_T_CHECK(t, len != 0u, "nested document did not fit");
    if (open_doc(t, &pl, doc, len, "64 nested arrays"))
        expect_st(t, vmfw_plist_array_count(&pl, "", &(size_t){0}),
                  VMFW_PLIST_ERR_TOO_DEEP, "count(64 nested)");
}

static void test_entities(vmfw_test_t *t) {
    VMFW_T_SECTION(t, "entities");

    vmfw_plist_t pl;

    if (open_doc(t, &pl, DOC("<plist version=\"1.0\"><dict>"
                             "<key>a&amp;b</key>"
                             "<string>x&lt;y&gt;z&quot;q&apos;r</string>"
                             "<key>plain</key><string>&amp;&amp;</string>"
                             "</dict></plist>"), "entities")) {
        /* The key is matched after decoding, so the caller writes the value it
         * would read, not the escape. */
        expect_str(t, &pl, "a&b", "x<y>z\"q'r");
        expect_str(t, &pl, "plain", "&&");
        /* The raw form does not match. */
        expect_str_st(t, &pl, "a&amp;b", VMFW_PLIST_ERR_NOT_FOUND);
    }

    /* Anything else is refused rather than passed through raw. */
    if (open_doc(t, &pl, DOC("<plist version=\"1.0\"><dict><key>a</key>"
                             "<string>&nbsp;</string></dict></plist>"),
                 "unknown entity"))
        expect_str_st(t, &pl, "a", VMFW_PLIST_ERR_MALFORMED);

    if (open_doc(t, &pl, DOC("<plist version=\"1.0\"><dict><key>a</key>"
                             "<string>&#65;</string></dict></plist>"),
                 "numeric reference"))
        expect_str_st(t, &pl, "a", VMFW_PLIST_ERR_MALFORMED);

    if (open_doc(t, &pl, DOC("<plist version=\"1.0\"><dict><key>a</key>"
                             "<string>&#0;</string></dict></plist>"),
                 "NUL reference"))
        expect_str_st(t, &pl, "a", VMFW_PLIST_ERR_MALFORMED);

    /* An entity with no terminator, and a bare ampersand. */
    if (open_doc(t, &pl, DOC("<plist version=\"1.0\"><dict><key>a</key>"
                             "<string>&amp</string></dict></plist>"),
                 "unterminated entity"))
        expect_str_st(t, &pl, "a", VMFW_PLIST_ERR_MALFORMED);

    if (open_doc(t, &pl, DOC("<plist version=\"1.0\"><dict><key>a</key>"
                             "<string>x &amp y</string></dict></plist>"),
                 "bare ampersand"))
        expect_str_st(t, &pl, "a", VMFW_PLIST_ERR_MALFORMED);

    /* An entity cut off by the end of the element. */
    if (open_doc(t, &pl, DOC("<plist version=\"1.0\"><dict><key>a</key>"
                             "<string>&am</string></dict></plist>"),
                 "entity across the end tag"))
        expect_str_st(t, &pl, "a", VMFW_PLIST_ERR_MALFORMED);

    /*
     * A bad entity in a key we are not asking for is still refused. Whether a
     * document parses must not depend on which key the caller wanted.
     */
    if (open_doc(t, &pl, DOC("<plist version=\"1.0\"><dict>"
                             "<key>&bogus;</key><string>v</string>"
                             "<key>a</key><string>b</string>"
                             "</dict></plist>"), "bad entity in another key"))
        expect_str_st(t, &pl, "a", VMFW_PLIST_ERR_MALFORMED);

    /* Entity decoding is length-aware: "&amp;" is one character, so a value of
     * two ampersands needs three bytes and not six. */
    if (open_doc(t, &pl, DOC("<plist version=\"1.0\"><dict><key>a</key>"
                             "<string>&amp;&amp;</string></dict></plist>"),
                 "entity length")) {
        char small[3];
        expect_st(t, vmfw_plist_get_string(&pl, "a", small, sizeof small),
                  VMFW_PLIST_OK, "two entities into 3 bytes");
        VMFW_T_EQ_STR(t, small, "&&", "decoded entity pair");
        expect_st(t, vmfw_plist_get_string(&pl, "a", small, 2u),
                  VMFW_PLIST_ERR_TOO_LONG, "two entities into 2 bytes");
    }
}

static void test_base64(vmfw_test_t *t) {
    VMFW_T_SECTION(t, "base64");

    uint8_t out[64];
    size_t n = 0;

    /* RFC 4648 section 10. */
    static const struct { const char *b64; const char *want; size_t len; }
    vectors[] = {
        { "",         "",       0u },
        { "Zg==",     "f",      1u },
        { "Zm8=",     "fo",     2u },
        { "Zm9v",     "foo",    3u },
        { "Zm9vYg==", "foob",   4u },
        { "Zm9vYmE=", "fooba",  5u },
        { "Zm9vYmFy", "foobar", 6u },
    };
    for (size_t i = 0; i < sizeof vectors / sizeof vectors[0]; i++) {
        const uint8_t *in = (const uint8_t *)vectors[i].b64;
        size_t in_len = 0;
        while (vectors[i].b64[in_len]) in_len++;
        n = 999u;
        expect_st(t, vmfw_base64_decode(in, in_len, out, sizeof out, &n),
                  VMFW_PLIST_OK, vectors[i].b64);
        VMFW_T_EQ_U(t, n, vectors[i].len, "vector length");
        /* decoded_size is exact for valid input: a caller that sizes a buffer
         * from it must not then be told the buffer is too small. */
        VMFW_T_EQ_U(t, vmfw_base64_decoded_size(in, in_len), vectors[i].len,
                    "vector decoded_size");
        if (n == vectors[i].len && n)
            VMFW_T_EQ_MEM(t, out, vectors[i].want, n, "vector bytes");
    }

    /* Whitespace is skipped wherever Apple's writer might put it. */
    expect_st(t, vmfw_base64_decode(DOC("Zm9v\n\tYmFy"), out, sizeof out, &n),
              VMFW_PLIST_OK, "newline+tab inside base64");
    VMFW_T_EQ_U(t, n, 6u, "wrapped length");
    VMFW_T_EQ_MEM(t, out, "foobar", 6u, "wrapped bytes");
    expect_st(t, vmfw_base64_decode(DOC(" \r\n\tZ m\t9\rv\nY m F y \n"),
                                    out, sizeof out, &n),
              VMFW_PLIST_OK, "whitespace everywhere");
    VMFW_T_EQ_U(t, n, 6u, "whitespace-shot length");
    VMFW_T_EQ_MEM(t, out, "foobar", 6u, "whitespace-shot bytes");
    VMFW_T_EQ_U(t, vmfw_base64_decoded_size(DOC(" \r\n\tZ m\t9\rv\nY m F y \n")),
                6u, "whitespace-shot decoded_size");

    /* Anything else is refused rather than ignored. */
    expect_st(t, vmfw_base64_decode(DOC("Zm9v*mFy"), out, sizeof out, &n),
              VMFW_PLIST_ERR_BAD_BASE64, "asterisk");
    /* The URL-safe alphabet is a different alphabet, not a lenient one. */
    expect_st(t, vmfw_base64_decode(DOC("Zm9v-mFy"), out, sizeof out, &n),
              VMFW_PLIST_ERR_BAD_BASE64, "urlsafe '-'");
    expect_st(t, vmfw_base64_decode(DOC("Zm9v_mFy"), out, sizeof out, &n),
              VMFW_PLIST_ERR_BAD_BASE64, "urlsafe '_'");
    expect_st(t, vmfw_base64_decode(DOC("Zm9v\0YmFy"), out, sizeof out, &n),
              VMFW_PLIST_ERR_BAD_BASE64, "embedded NUL");
    expect_st(t, vmfw_base64_decode(DOC("Zm9vYmF\x80"), out, sizeof out, &n),
              VMFW_PLIST_ERR_BAD_BASE64, "high byte");

    /* Padding. */
    expect_st(t, vmfw_base64_decode(DOC("Zm9vYg="), out, sizeof out, &n),
              VMFW_PLIST_ERR_BAD_BASE64, "7 characters");
    expect_st(t, vmfw_base64_decode(DOC("Zm8"), out, sizeof out, &n),
              VMFW_PLIST_ERR_BAD_BASE64, "3 characters");
    expect_st(t, vmfw_base64_decode(DOC("Z"), out, sizeof out, &n),
              VMFW_PLIST_ERR_BAD_BASE64, "1 character");
    expect_st(t, vmfw_base64_decode(DOC("Zg==="), out, sizeof out, &n),
              VMFW_PLIST_ERR_BAD_BASE64, "three pad characters");
    expect_st(t, vmfw_base64_decode(DOC("Zg==Zg=="), out, sizeof out, &n),
              VMFW_PLIST_ERR_BAD_BASE64, "data after padding");
    expect_st(t, vmfw_base64_decode(DOC("Z=g="), out, sizeof out, &n),
              VMFW_PLIST_ERR_BAD_BASE64, "padding in the middle");
    expect_st(t, vmfw_base64_decode(DOC("=Zm8"), out, sizeof out, &n),
              VMFW_PLIST_ERR_BAD_BASE64, "leading padding");
    expect_st(t, vmfw_base64_decode(DOC("===="), out, sizeof out, &n),
              VMFW_PLIST_ERR_BAD_BASE64, "all padding");
    /*
     * These three are the ones a padding-bit check cannot catch for you: every
     * quantum here decodes to zero bits, so only the rule that data may not
     * follow '=' refuses them. Without it "AA=A" yields two bytes out of a
     * quantum that declared it held one.
     */
    expect_st(t, vmfw_base64_decode(DOC("AA=A"), out, sizeof out, &n),
              VMFW_PLIST_ERR_BAD_BASE64, "data after padding, all-zero bits");
    expect_st(t, vmfw_base64_decode(DOC("A=AA"), out, sizeof out, &n),
              VMFW_PLIST_ERR_BAD_BASE64, "padding at position 1, all-zero bits");
    expect_st(t, vmfw_base64_decode(DOC("AAA=AAAA"), out, sizeof out, &n),
              VMFW_PLIST_ERR_BAD_BASE64, "quantum after a padded one");
    /* The unpadded forms of the same bits are fine. */
    expect_st(t, vmfw_base64_decode(DOC("AAA="), out, sizeof out, &n),
              VMFW_PLIST_OK, "AAA= is well formed");
    VMFW_T_EQ_U(t, n, 2u, "AAA= length");
    expect_st(t, vmfw_base64_decode(DOC("AAAAAAAA"), out, sizeof out, &n),
              VMFW_PLIST_OK, "eight zeros");
    VMFW_T_EQ_U(t, n, 6u, "eight zeros length");

    /*
     * Padding bits. "Zg==" and "Zh==" both decode to "f" under a lenient
     * decoder, which makes the encoding non-injective: two documents, same
     * bytes. Every correct encoder writes those spare bits as zero, and all 76
     * <data> blocks in the 7E18 BuildManifesto.plist re-encode byte for byte,
     * so refusing a non-zero tail rejects none of them.
     */
    expect_st(t, vmfw_base64_decode(DOC("Zg=="), out, sizeof out, &n),
              VMFW_PLIST_OK, "canonical 4-bit tail");
    expect_st(t, vmfw_base64_decode(DOC("Zh=="), out, sizeof out, &n),
              VMFW_PLIST_ERR_BAD_BASE64, "non-zero 4-bit tail");
    expect_st(t, vmfw_base64_decode(DOC("QQ=="), out, sizeof out, &n),
              VMFW_PLIST_OK, "canonical 4-bit tail (QQ)");
    expect_st(t, vmfw_base64_decode(DOC("QR=="), out, sizeof out, &n),
              VMFW_PLIST_ERR_BAD_BASE64, "non-zero 4-bit tail (QR)");
    expect_st(t, vmfw_base64_decode(DOC("Zm8="), out, sizeof out, &n),
              VMFW_PLIST_OK, "canonical 2-bit tail");
    expect_st(t, vmfw_base64_decode(DOC("Zm9="), out, sizeof out, &n),
              VMFW_PLIST_ERR_BAD_BASE64, "non-zero 2-bit tail");

    /* Capacity. */
    expect_st(t, vmfw_base64_decode(DOC("Zm9vYmFy"), out, 5u, &n),
              VMFW_PLIST_ERR_TOO_LONG, "6 bytes into 5");
    VMFW_T_EQ_U(t, n, 0u, "refused decode reported no bytes");
    expect_st(t, vmfw_base64_decode(DOC("Zm9vYmFy"), out, 6u, &n),
              VMFW_PLIST_OK, "6 bytes into 6");
    VMFW_T_EQ_U(t, n, 6u, "exact-fit length");
    expect_st(t, vmfw_base64_decode(DOC("Zg=="), NULL, 0u, &n),
              VMFW_PLIST_ERR_TOO_LONG, "1 byte into 0");

    /* Caller bugs. */
    expect_st(t, vmfw_base64_decode(NULL, 4u, out, sizeof out, &n),
              VMFW_PLIST_ERR_INVALID_ARGUMENT, "decode(in=NULL)");
    expect_st(t, vmfw_base64_decode(DOC("Zm9v"), NULL, 4u, &n),
              VMFW_PLIST_ERR_INVALID_ARGUMENT, "decode(out=NULL, cap>0)");
    expect_st(t, vmfw_base64_decode(NULL, 0u, NULL, 0u, NULL),
              VMFW_PLIST_OK, "decode(nothing)");

    /* decoded_size counts only alphabet characters, and never over-reports. */
    VMFW_T_EQ_U(t, vmfw_base64_decoded_size(NULL, 8u), 0u, "size(NULL)");
    VMFW_T_EQ_U(t, vmfw_base64_decoded_size(DOC("")), 0u, "size(empty)");
    VMFW_T_EQ_U(t, vmfw_base64_decoded_size(DOC("\n\t\r ")), 0u,
                "size(whitespace only)");
    VMFW_T_EQ_U(t, vmfw_base64_decoded_size(DOC("****")), 0u,
                "size(illegal only)");
    VMFW_T_EQ_U(t, vmfw_base64_decoded_size(DOC("Zg==")), 1u, "size(Zg==)");
    VMFW_T_EQ_U(t, vmfw_base64_decoded_size(DOC("Zm8=")), 2u, "size(Zm8=)");
    VMFW_T_EQ_U(t, vmfw_base64_decoded_size(DOC("Zm9v")), 3u, "size(Zm9v)");

    /*
     * Sizing then decoding must agree for every prefix of a real blob: whenever
     * a prefix decodes, the size reported for it is exactly what came out.
     */
    static const char blob[] =
        "TWFuIGlzIGRpc3Rpbmd1aXNoZWQsIG5vdCBvbmx5IGJ5IGhpcyByZWFzb24sIGJ1\n"
        "dCBieSB0aGlzIHNpbmd1bGFyIHBhc3Npb24gZnJvbSBvdGhlciBhbmltYWxzLg==";
    uint8_t big[128];
    unsigned mismatches = 0;
    unsigned decoded_ok = 0;
    for (size_t cut = 0; cut <= sizeof blob - 1u; cut++) {
        const uint8_t *in = (const uint8_t *)blob;
        size_t got = 0;
        const size_t sized = vmfw_base64_decoded_size(in, cut);
        if (vmfw_base64_decode(in, cut, big, sizeof big, &got)
            == VMFW_PLIST_OK) {
            decoded_ok++;
            if (sized != got) mismatches++;
        }
    }
    VMFW_T_EQ_U(t, mismatches, 0u, "prefix size/decode disagreements");
    VMFW_T_CHECK(t, decoded_ok > 8u,
                 "only %u prefixes decoded; the sweep proved nothing",
                 decoded_ok);
}

/* ------------------------------------------------------------------------ */
/* Truncation                                                                */
/* ------------------------------------------------------------------------ */

/*
 * Feed every prefix of a document through init and all four accessors. A prefix
 * may be refused for any reason, but it may never report success with a value
 * that differs from the whole document's, and it may never read past the length
 * it was given.
 */
static void sweep_all_prefixes(vmfw_test_t *t, const char *xml, size_t len,
                               const char *label,
                               const char *spath, const char *sval,
                               const char *apath, size_t aval,
                               const char *dpath) {
    unsigned wrong = 0;
    unsigned opened = 0;
    static uint8_t scratch[4096];

    for (size_t cut = 0; cut <= len; cut++) {
        vmfw_plist_t pl;
        if (vmfw_plist_init(&pl, (const uint8_t *)xml, cut) != VMFW_PLIST_OK)
            continue;
        opened++;

        char buf[256];
        if (vmfw_plist_get_string(&pl, spath, buf, sizeof buf)
            == VMFW_PLIST_OK && strcmp(buf, sval) != 0) wrong++;

        size_t count = 0;
        if (vmfw_plist_array_count(&pl, apath, &count) == VMFW_PLIST_OK &&
            count != aval) wrong++;

        const uint8_t *span = NULL;
        size_t span_len = 0;
        if (vmfw_plist_get_data_span(&pl, dpath, &span, &span_len)
            == VMFW_PLIST_OK) {
            if (span + span_len > (const uint8_t *)xml + cut) wrong++;
            size_t got = 0;
            (void)vmfw_base64_decode(span, span_len, scratch,
                                     sizeof scratch, &got);
        }
    }

    VMFW_T_EQ_U(t, wrong, 0u, label);
    /* The whole document must at least be one of the prefixes that opened. */
    VMFW_T_CHECK(t, opened > 0u, "%s: no prefix opened at all", label);
}

static void test_truncation(vmfw_test_t *t) {
    VMFW_T_SECTION(t, "truncation");

    /*
     * Thirty-two named offsets across each document, checked one at a time so a
     * regression names the offset it broke at, followed by the exhaustive sweep
     * over every prefix length.
     */
    static const char *const labels[2] = { "Restore.plist", "resource fork" };
    const char *const docs[2] = { k_restore, k_fork };
    const size_t lens[2] = { sizeof k_restore - 1u, sizeof k_fork - 1u };
    const char *const paths[2] = { "ProductType", "resource-fork/blkx/5/Name" };
    const char *const vals[2] = { "iPhone1,2", "disk image (Apple_HFSX : 5)" };

    for (unsigned d = 0; d < 2u; d++) {
        for (unsigned i = 0; i < 32u; i++) {
            const size_t cut = (lens[d] * i) / 32u;
            vmfw_plist_t pl;
            const vmfw_plist_status_t ist =
                vmfw_plist_init(&pl, (const uint8_t *)docs[d], cut);
            if (ist != VMFW_PLIST_OK) {
                VMFW_T_CHECK(t, ist == VMFW_PLIST_ERR_NOT_XML ||
                                ist == VMFW_PLIST_ERR_MALFORMED,
                             "%s cut %u: init -> %s", labels[d], i,
                             vmfw_plist_strerror(ist));
                continue;
            }
            char buf[256];
            const vmfw_plist_status_t st =
                vmfw_plist_get_string(&pl, paths[d], buf, sizeof buf);
            VMFW_T_CHECK(t, st != VMFW_PLIST_OK || strcmp(buf, vals[d]) == 0,
                         "%s cut %u: succeeded with \"%s\"", labels[d], i, buf);
        }
    }

    sweep_all_prefixes(t, k_restore, sizeof k_restore - 1u,
                       "Restore.plist prefixes",
                       "ProductType", "iPhone1,2",
                       "DeviceMap", 1u,
                       "SystemRestoreImages/User");

    sweep_all_prefixes(t, k_fork, sizeof k_fork - 1u,
                       "resource fork prefixes",
                       "resource-fork/blkx/5/Name", "disk image (Apple_HFSX : 5)",
                       "resource-fork/blkx", FORK_ENTRIES,
                       "resource-fork/blkx/0/Data");

    sweep_all_prefixes(t, k_wide_data, sizeof k_wide_data - 1u,
                       "64-column data prefixes",
                       "Missing", "",
                       "Missing", 0u,
                       "Blob");
}

static void test_strerror(vmfw_test_t *t) {
    VMFW_T_SECTION(t, "strerror");

    /* Every status has to say something, or a failure reaches the user as a
     * number. */
    for (int i = 0; i <= (int)VMFW_PLIST_ERR_BAD_BASE64; i++) {
        const char *s = vmfw_plist_strerror((vmfw_plist_status_t)i);
        VMFW_T_CHECK(t, s != NULL && s[0] != '\0' &&
                        strcmp(s, "unknown error") != 0,
                     "strerror(%d) = \"%s\"", i, s ? s : "(null)");
    }
    VMFW_T_EQ_STR(t, vmfw_plist_strerror((vmfw_plist_status_t)999),
                  "unknown error", "strerror(999)");
}

/* ------------------------------------------------------------------------ */

void vmfw_test_plist(vmfw_test_t *t) {
    test_init(t);
    test_restore(t);
    test_restore_errors(t);
    test_fork(t);
    test_fork_data(t);
    test_structure(t);
    test_self_closing(t);
    test_depth(t);
    test_entities(t);
    test_base64(t);
    test_truncation(t);
    test_strerror(t);
    VMFW_T_SECTION(t, NULL);
}
