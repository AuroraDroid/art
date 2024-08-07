/*
 * Copyright (C) 2012 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef ART_RUNTIME_GC_ACCOUNTING_MOD_UNION_TABLE_H_
#define ART_RUNTIME_GC_ACCOUNTING_MOD_UNION_TABLE_H_

#include "base/allocator.h"
#include "base/safe_map.h"
#include "base/tracking_safe_map.h"
#include "bitmap.h"
#include "card_table.h"
#include "mirror/object_reference.h"
#include "runtime_globals.h"

#include <set>
#include <vector>

namespace art HIDDEN {

namespace mirror {
class Object;
}  // namespace mirror

class MarkObjectVisitor;

namespace gc {
namespace space {
class ContinuousSpace;
}  // namespace space

class Heap;

namespace accounting {

// The mod-union table is the union of modified cards. It is used to allow the card table to be
// cleared between GC phases, reducing the number of dirty cards that need to be scanned.
class ModUnionTable {
 public:
  // A callback for visiting an object in the heap.
  using ObjectCallback = void (*)(mirror::Object*, void*);

  using CardSet = std::set<uint8_t*,
                           std::less<uint8_t*>,
                           TrackingAllocator<uint8_t*, kAllocatorTagModUnionCardSet>>;
  using CardBitmap = MemoryRangeBitmap<CardTable::kCardSize>;

  explicit ModUnionTable(const std::string& name, Heap* heap, space::ContinuousSpace* space)
      : name_(name),
        heap_(heap),
        space_(space) {}

  virtual ~ModUnionTable() {}

  // Process cards for a memory range of a space. This doesn't immediately update the mod-union
  // table, as updating the mod-union table may have an associated cost, such as determining
  // references to track.
  virtual void ProcessCards() = 0;

  // Set all the cards.
  virtual void SetCards() = 0;

  // Clear all of the table.
  virtual void ClearTable() = 0;

  // Update the mod-union table using data stored by ProcessCards. There may be multiple
  // ProcessCards before a call to update, for example, back-to-back sticky GCs. Also mark
  // references to other spaces which are stored in the mod-union table.
  virtual void UpdateAndMarkReferences(MarkObjectVisitor* visitor) = 0;

  // Visit all of the objects that may contain references to other spaces.
  virtual void VisitObjects(ObjectCallback callback, void* arg) = 0;

  // Verification: consistency checks that we don't have clean cards which conflict with out
  // cached data for said cards. Exclusive lock is required since verify sometimes uses
  // SpaceBitmap::VisitMarkedRange and VisitMarkedRange can't know if the callback will modify the
  // bitmap or not.
  virtual void Verify() REQUIRES(Locks::heap_bitmap_lock_) = 0;

  // Returns true if a card is marked inside the mod union table. Used for testing. The address
  // doesn't need to be aligned.
  virtual bool ContainsCardFor(uintptr_t addr) = 0;

  // Filter out cards that don't need to be marked. Automatically done with UpdateAndMarkReferences.
  void FilterCards();

  virtual void Dump(std::ostream& os) = 0;

  space::ContinuousSpace* GetSpace() {
    return space_;
  }

  Heap* GetHeap() const {
    return heap_;
  }

  const std::string& GetName() const {
    return name_;
  }

 protected:
  const std::string name_;
  Heap* const heap_;
  space::ContinuousSpace* const space_;
};

// Reference caching implementation. Caches references pointing to alloc space(s) for each card.
class ModUnionTableReferenceCache : public ModUnionTable {
 public:
  explicit ModUnionTableReferenceCache(const std::string& name, Heap* heap,
                                       space::ContinuousSpace* space)
      : ModUnionTable(name, heap, space) {}

  virtual ~ModUnionTableReferenceCache() {}

  // Clear and store cards for a space.
  void ProcessCards() override;

  // Update table based on cleared cards and mark all references to the other spaces.
  void UpdateAndMarkReferences(MarkObjectVisitor* visitor) override
      REQUIRES_SHARED(Locks::mutator_lock_)
      REQUIRES(Locks::heap_bitmap_lock_);

  void VisitObjects(ObjectCallback callback, void* arg) override
      REQUIRES(Locks::heap_bitmap_lock_)
      REQUIRES_SHARED(Locks::mutator_lock_);

  // Exclusive lock is required since verify uses SpaceBitmap::VisitMarkedRange and
  // VisitMarkedRange can't know if the callback will modify the bitmap or not.
  void Verify() override
      REQUIRES_SHARED(Locks::mutator_lock_)
      REQUIRES(Locks::heap_bitmap_lock_);

  // Function that tells whether or not to add a reference to the table.
  virtual bool ShouldAddReference(const mirror::Object* ref) const = 0;

  bool ContainsCardFor(uintptr_t addr) override;

  void Dump(std::ostream& os) override REQUIRES_SHARED(Locks::mutator_lock_);

  void SetCards() override;

  void ClearTable() override;

 protected:
  // Cleared card array, used to update the mod-union table.
  ModUnionTable::CardSet cleared_cards_;

  // Maps from dirty cards to their corresponding alloc space references.
  AllocationTrackingSafeMap<const uint8_t*, std::vector<mirror::HeapReference<mirror::Object>*>,
                            kAllocatorTagModUnionReferenceArray> references_;
};

// Card caching implementation. Keeps track of which cards we cleared and only this information.
class ModUnionTableCardCache : public ModUnionTable {
 public:
  // Note: There is assumption that the space End() doesn't change.
  explicit ModUnionTableCardCache(const std::string& name, Heap* heap,
                                  space::ContinuousSpace* space);

  virtual ~ModUnionTableCardCache() {}

  // Clear and store cards for a space.
  void ProcessCards() override;

  // Mark all references to the alloc space(s).
  void UpdateAndMarkReferences(MarkObjectVisitor* visitor) override
      REQUIRES(Locks::heap_bitmap_lock_)
      REQUIRES_SHARED(Locks::mutator_lock_);

  void VisitObjects(ObjectCallback callback, void* arg) override
      REQUIRES(Locks::heap_bitmap_lock_)
      REQUIRES_SHARED(Locks::mutator_lock_);

  // Nothing to verify.
  void Verify() override {}

  void Dump(std::ostream& os) override;

  bool ContainsCardFor(uintptr_t addr) override;

  void SetCards() override;

  void ClearTable() override;

 protected:
  // Cleared card bitmap, used to update the mod-union table.
  std::unique_ptr<CardBitmap> card_bitmap_;
};

}  // namespace accounting
}  // namespace gc
}  // namespace art

#endif  // ART_RUNTIME_GC_ACCOUNTING_MOD_UNION_TABLE_H_
