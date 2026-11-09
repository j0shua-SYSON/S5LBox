// Copyright (c) 2026 j0shua-SYSON. MIT licensed.
//! Sparse host backing for the guest VM allocator. Guest addresses remain
//! 32-bit; host allocations cover only live regions, not a flat 4 GiB array.
//! Mappings never move while live, including after a partial guest unmap.
use std::collections::BTreeMap;
use std::rc::Rc;

struct Allocation {
    pointer: *mut u8,
    size: usize,
}
impl Drop for Allocation {
    fn drop(&mut self) {
        // Never unwind from cleanup while containing a failed guest session.
        let _ = unsafe { super::host::free_memory(self.pointer.cast(), self.size) };
    }
}
struct Region {
    allocation: Rc<Allocation>,
    offset: usize,
    size: u32,
}
#[derive(Default)]
pub(super) struct Backing {
    regions: BTreeMap<u32, Region>,
}
impl Backing {
    pub fn map(&mut self, base: u32, size: u32) -> std::io::Result<()> {
        assert!(size > 0 && base as u64 + size as u64 <= 1u64 << 32);
        let end = base as u64 + size as u64;
        for (&previous, region) in &self.regions {
            assert!(
                previous as u64 + region.size as u64 <= base as u64 || previous as u64 >= end,
                "overlapping live guest backing"
            );
        }
        let pointer = unsafe { super::host::allocate_memory(size as usize)? }.cast::<u8>();
        assert_eq!(pointer as usize & super::PAGE_SIZE_ALIGN_MASK as usize, 0);
        self.regions.insert(
            base,
            Region {
                allocation: Rc::new(Allocation {
                    pointer,
                    size: size as usize,
                }),
                offset: 0,
                size,
            },
        );
        Ok(())
    }

    pub fn unmap(&mut self, base: u32, size: u32) {
        let end = base as u64 + size as u64;
        assert!(end <= 1u64 << 32);
        let keys: Vec<u32> = self
            .regions
            .iter()
            .filter_map(|(&start, region)| {
                ((start as u64) < end && start as u64 + region.size as u64 > base as u64)
                    .then_some(start)
            })
            .collect();
        for start in keys {
            let region = self.regions.remove(&start).unwrap();
            let old_end = start as u64 + region.size as u64;
            if start < base {
                self.regions.insert(
                    start,
                    Region {
                        allocation: region.allocation.clone(),
                        offset: region.offset,
                        size: base - start,
                    },
                );
            }
            if old_end > end {
                self.regions.insert(
                    end as u32,
                    Region {
                        allocation: region.allocation,
                        offset: region.offset + (end - start as u64) as usize,
                        size: (old_end - end) as u32,
                    },
                );
            }
        }
    }

    pub(super) fn pointer(&self, address: u32, count: u32) -> Option<*mut u8> {
        let (&base, region) = self.regions.range(..=address).next_back()?;
        let offset = address.checked_sub(base)? as u64;
        if offset + count as u64 > region.size as u64 {
            return None;
        }
        // At count=0, allow the one-past pointer of a mapped allocation.
        Some(unsafe {
            region
                .allocation
                .pointer
                .add(region.offset + offset as usize)
        })
    }
    pub fn bytes(&self, address: u32, count: u32) -> Option<&[u8]> {
        let pointer = self.pointer(address, count)?;
        Some(unsafe { std::slice::from_raw_parts(pointer, count as usize) })
    }
    pub fn bytes_mut(&mut self, address: u32, count: u32) -> Option<&mut [u8]> {
        let pointer = self.pointer(address, count)?;
        Some(unsafe { std::slice::from_raw_parts_mut(pointer, count as usize) })
    }
    pub fn guest_pointer(&self, pointer: *const std::ffi::c_void) -> Option<u32> {
        let pointer = pointer as usize;
        for (&base, region) in &self.regions {
            let start = region.allocation.pointer as usize + region.offset;
            if pointer >= start && pointer < start + region.size as usize {
                return Some(base + (pointer - start) as u32);
            }
        }
        None
    }

    pub fn copy_contiguous(&mut self, source: u32, destination: u32, size: u32) -> bool {
        let (Some(source), Some(destination)) =
            (self.pointer(source, size), self.pointer(destination, size))
        else {
            return false;
        };
        // Use the original allocation pointers, not a pointer derived from a
        // shared slice that would forbid this potentially overlapping write.
        unsafe {
            std::ptr::copy(source, destination, size as usize);
        }
        true
    }

    /// CPU callbacks may cross adjacent mappings. Native borrowed slices must
    /// remain within one backing region; allocations themselves are contiguous.
    pub fn read(&self, address: u32, bytes: &mut [u8]) -> bool {
        if address as u64 + bytes.len() as u64 > 1u64 << 32 {
            return false;
        }
        let mut offset = 0;
        while offset < bytes.len() {
            let addr = (address as u64 + offset as u64) as u32;
            let Some((&base, region)) = self.regions.range(..=addr).next_back() else {
                return false;
            };
            let available = (base as u64 + region.size as u64).saturating_sub(addr as u64) as usize;
            if available == 0 {
                return false;
            }
            let count = available.min(bytes.len() - offset);
            bytes[offset..offset + count].copy_from_slice(self.bytes(addr, count as u32).unwrap());
            offset += count;
        }
        true
    }
    pub fn write(&mut self, address: u32, bytes: &[u8]) -> bool {
        // Check the entire range first so a fault cannot cause a partial store.
        if !self.is_mapped(address, bytes.len()) {
            return false;
        }
        let mut offset = 0;
        while offset < bytes.len() {
            let addr = (address as u64 + offset as u64) as u32;
            let (&base, region) = self.regions.range(..=addr).next_back().unwrap();
            let count = ((base as u64 + region.size as u64 - addr as u64) as usize)
                .min(bytes.len() - offset);
            self.bytes_mut(addr, count as u32)
                .unwrap()
                .copy_from_slice(&bytes[offset..offset + count]);
            offset += count;
        }
        true
    }

    pub fn is_mapped(&self, address: u32, length: usize) -> bool {
        if address as u64 + length as u64 > 1u64 << 32 {
            return false;
        }
        let mut offset = 0;
        while offset < length {
            let addr = (address as u64 + offset as u64) as u32;
            let Some((&base, region)) = self.regions.range(..=addr).next_back() else {
                return false;
            };
            let available = (base as u64 + region.size as u64).saturating_sub(addr as u64) as usize;
            if available == 0 {
                return false;
            }
            offset += available.min(length - offset);
        }
        true
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn sparse_high_addresses_and_stable_native_pointers() {
        let mut map = Backing::default();
        map.map(0x1000, 0x1000).unwrap();
        map.map(0xffff0000, 0x10000).unwrap();
        let pointer = map.bytes(0x1000, 1).unwrap().as_ptr();
        map.map(0x30000000, 0x8000).unwrap();
        assert_eq!(map.bytes(0x1000, 1).unwrap().as_ptr(), pointer);
        assert_eq!(map.guest_pointer(pointer.cast()), Some(0x1000));
        assert!(map.write(0xfffffffc, &[1, 2, 3, 4]));
        assert!(!map.write(0xfffffffe, &[1, 2, 3, 4]));
        assert_eq!(map.bytes(0xfffffffc, 4).unwrap(), &[1, 2, 3, 4]);
        assert!(map.bytes(0x2000, 1).is_none());
    }
    #[test]
    fn adjacent_regions_and_holes_have_atomic_faulting_stores() {
        let mut map = Backing::default();
        map.map(0x1000, 0x1000).unwrap();
        map.map(0x2000, 0x1000).unwrap();
        assert!(map.write(0x1ffe, &[1, 2, 3, 4]));
        let mut bytes = [0; 4];
        assert!(map.read(0x1ffe, &mut bytes));
        assert_eq!(bytes, [1, 2, 3, 4]);
        map.unmap(0x2000, 0x1000);
        assert!(!map.write(0x1ffe, &[9, 9, 9, 9]));
        assert_eq!(map.bytes(0x1ffe, 2).unwrap(), &[1, 2]);
    }
    #[test]
    fn partial_unmap_keeps_other_native_pointers_stable() {
        let mut map = Backing::default();
        map.map(0x1000, 0x3000).unwrap();
        let left = map.bytes(0x1000, 1).unwrap().as_ptr();
        let right = map.bytes(0x3000, 1).unwrap().as_ptr();
        map.unmap(0x2000, 0x1000);
        map.map(0x2000, 0x1000).unwrap();
        assert_eq!(map.bytes(0x1000, 1).unwrap().as_ptr(), left);
        assert_eq!(map.bytes(0x3000, 1).unwrap().as_ptr(), right);
        assert_eq!(map.bytes(0x2000, 4).unwrap(), &[0; 4]);
    }
}
