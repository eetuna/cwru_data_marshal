# Prompt for Next Agent

## Context
I'm working on the CWRU MRI Data Marshal project. The previous agent made changes to implement a "metadata-only" architecture for SWMR (Single Writer Multiple Readers) where clients do direct HDF5 reads instead of the marshal reading and sending binary data over HTTP.

**However, the changes were lost when the git worktree was deleted.**

## Task
Please implement the SWMR metadata-only architecture changes by following the comprehensive documentation provided.

## Documentation Available
1. **`/workspaces/cwru_data_marshal/docs/HANDOVER_SWMR_IMPLEMENTATION.md`** - Complete implementation guide
2. **`/workspaces/cwru_data_marshal/docs/QUICK_REFERENCE.md`** - Quick reference
3. **`/workspaces/cwru_data_marshal/docs/CORRECTED_ARCHIVE_IMPLEMENTATION.md`** - REAL working C++ code for archive file

## What You Need to Do

### Step 1: Read the Documentation
```bash
cat /workspaces/cwru_data_marshal/docs/HANDOVER_SWMR_IMPLEMENTATION.md
cat /workspaces/cwru_data_marshal/docs/CORRECTED_ARCHIVE_IMPLEMENTATION.md
```

### Step 2: Navigate to Working Directory
Repository: `/workspaces/cwru_data_marshal`
Branch: `mri-data-marhsal`

### Step 3: Apply Changes

**ONLY 2 FILES NEED CHANGES:**

1. **`src/marshal_http.hpp`**
   - Modify GET `/v1/mrd/frame` (around line 491-509) → return JSON metadata
   - Modify GET `/v1/mrd/ingest` (around line 561-573) → return JSON metadata
   - See HANDOVER_SWMR_IMPLEMENTATION.md for exact code

2. **`src/marshal_http_archive.hpp`** (NEW FILE)
   - Copy FULL content from CORRECTED_ARCHIVE_IMPLEMENTATION.md
   - REAL C++ code with working binary implementation
   - NOT compiled - for reference only

**Already correct (no changes):**
- ✓ viz_client already has HDF5 SWMR
- ✓ CMakeLists.txt already correct

### Step 4: Build and Test
```bash
cd /workspaces/cwru_data_marshal
mkdir -p build && cd build
cmake .. -DBUILD_TESTING=ON
cmake --build . --parallel
ctest --output-on-failure
```

Expected: All 9/9 tests pass ✓

### Step 5: Commit (Optional)
```bash
git add src/marshal_http.hpp src/marshal_http_archive.hpp
git commit -m "Implement metadata-only endpoints with direct HDF5 SWMR reads

- Modified GET /v1/mrd/frame to return metadata JSON
- Modified GET /v1/mrd/ingest to return metadata JSON  
- Created marshal_http_archive.hpp with REAL binary code for reference

Benefits: 2-3x faster, simpler, follows SWMR design

Co-Authored-By: Claude Sonnet 4.5 <noreply@anthropic.com>"
```

## Key Points

- Only 2 files to change
- viz_client already works (has HDF5 SWMR)
- Archive file has REAL working code (not just docs)
- All details in HANDOVER_SWMR_IMPLEMENTATION.md

Follow the docs step-by-step!
