// File Storage B+ Tree (BPT) key-value database.
//
// Composite entries (index string, int value) are kept sorted in an on-disk
// B+ tree. Leaves form a doubly linked list for range scans (find). The data
// file persists across program runs: if it exists it is reopened, otherwise a
// new tree is created.

#include <cstdio>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <climits>
#include <unordered_map>

namespace {

constexpr int PAGE_SZ = 8192;
constexpr int KLEN = 64;

constexpr uint32_t PG_LEAF = 1;
constexpr uint32_t PG_INTERNAL = 2;
constexpr uint32_t HDR_MAGIC = 0x42505431u; // "BPT1"

// Composite key: (index, value). index stored as raw bytes + length.
struct Entry {
  char key[KLEN];
  int32_t len;
  int32_t value;
};
static_assert(sizeof(Entry) == 72, "Entry must be 72 bytes");

// Leaf page layout:
//   [0..3]  type  [4..7] count  [8..11] next  [12..15] prev
//   entries at offset 16
constexpr int L_HEAD = 16;
constexpr int MAX_L = (PAGE_SZ - L_HEAD) / (int)sizeof(Entry); // 113
constexpr int MIN_L = (MAX_L + 1) / 2;                          // 57

// Internal page layout:
//   [0..3] type  [4..7] key count
//   children: MAX_IK+1 ints at offset 8 (reserved)
//   keys at offset I_KOFF
constexpr int MAX_IK = 107;
constexpr int MIN_IK = MAX_IK / 2; // 53
constexpr int I_KOFF = 8 + (MAX_IK + 1) * 4;
static_assert(I_KOFF + MAX_IK * (int)sizeof(Entry) <= PAGE_SZ, "internal node fits");
static_assert(L_HEAD + MAX_L * (int)sizeof(Entry) <= PAGE_SZ, "leaf fits");

inline int cmp_entry(const Entry &a, const Entry &b) {
  int n = a.len < b.len ? a.len : b.len;
  int c = memcmp(a.key, b.key, n);
  if (c != 0) return c < 0 ? -1 : 1;
  if (a.len != b.len) return a.len < b.len ? -1 : 1;
  if (a.value != b.value) return a.value < b.value ? -1 : 1;
  return 0;
}

inline bool same_index(const Entry &e, const char *idx, int ilen) {
  return e.len == ilen && memcmp(e.key, idx, ilen) == 0;
}

// ---- page field accessors (buffers are 8-byte aligned) ----
inline uint32_t pg_type(const uint8_t *p) { return *reinterpret_cast<const uint32_t *>(p); }
inline uint32_t &lcount(uint8_t *p) { return *reinterpret_cast<uint32_t *>(p + 4); }
inline int32_t &lnext(uint8_t *p) { return *reinterpret_cast<int32_t *>(p + 8); }
inline int32_t &lprev(uint8_t *p) { return *reinterpret_cast<int32_t *>(p + 12); }
inline Entry *lentry(uint8_t *p, int i) {
  return reinterpret_cast<Entry *>(p + L_HEAD + (int)sizeof(Entry) * i);
}
inline uint32_t &icount(uint8_t *p) { return *reinterpret_cast<uint32_t *>(p + 4); }
inline int32_t &ichild(uint8_t *p, int i) {
  return *reinterpret_cast<int32_t *>(p + 8 + 4 * i);
}
inline Entry *ikey(uint8_t *p, int i) {
  return reinterpret_cast<Entry *>(p + I_KOFF + (int)sizeof(Entry) * i);
}

// ---------------------------------------------------------------------------
// Buffer pool: fixed-size LRU page cache with pinning, backed by one file.
// ---------------------------------------------------------------------------
constexpr int NCACHE = 3072; // 3072 * 8 KiB = 24 MiB

class BufferPool {
public:
  FILE *fp = nullptr;
  int root_pid = -1;

  void init(const char *path) {
    fp = fopen(path, "rb+");
    bool fresh = (fp == nullptr);
    if (fresh) fp = fopen(path, "wb+");
    bool ok = false;
    if (fp != nullptr && !fresh) {
      uint8_t hdr[PAGE_SZ];
      read_page(0, hdr);
      uint32_t m;
      memcpy(&m, hdr, 4);
      if (m == HDR_MAGIC) {
        int32_t r, ff;
        uint32_t np;
        memcpy(&r, hdr + 8, 4);
        memcpy(&np, hdr + 12, 4);
        memcpy(&ff, hdr + 16, 4);
        root_pid = r;
        npages = np;
        first_free = ff;
        ok = true;
      }
    }
    if (!ok) {
      npages = 1; // page 0 is the header page
      first_free = -1;
      root_pid = (int)npages++;
      uint8_t *p = fetch(root_pid);
      *reinterpret_cast<uint32_t *>(p) = PG_LEAF;
      lcount(p) = 0;
      lnext(p) = -1;
      lprev(p) = -1;
      mark_dirty(p);
      unpin(root_pid);
    }
  }

  uint8_t *fetch(int pid) {
    auto it = mp.find(pid);
    int s;
    if (it != mp.end()) {
      s = it->second;
      pin_cnt[s]++;
      detach(s);
      push_front(s);
      return buf + (size_t)s * PAGE_SZ;
    }
    if (nslots < NCACHE) {
      s = nslots++;
      slot_page[s] = -1;
      pin_cnt[s] = 0;
      dirty_flag[s] = 0;
      prv[s] = nxt[s] = -1;
    } else {
      s = tail;
      while (s != -1 && pin_cnt[s] > 0) s = prv[s];
      if (s == -1) {
        // Should be impossible (max simultaneous pins ~ 2*height).
        fprintf(stderr, "bpt: cache deadlock\n");
        exit(1);
      }
      detach(s);
      if (dirty_flag[s]) write_page(slot_page[s], buf + (size_t)s * PAGE_SZ);
      mp.erase(slot_page[s]);
      slot_page[s] = -1;
      dirty_flag[s] = 0;
    }
    read_page(pid, buf + (size_t)s * PAGE_SZ);
    slot_page[s] = pid;
    pin_cnt[s] = 1;
    mp[pid] = s;
    push_front(s);
    return buf + (size_t)s * PAGE_SZ;
  }

  void unpin(int pid) {
    auto it = mp.find(pid);
    if (it != mp.end() && pin_cnt[it->second] > 0) pin_cnt[it->second]--;
  }

  void mark_dirty(uint8_t *p) { dirty_flag[(p - buf) / PAGE_SZ] = 1; }

  int alloc_page() {
    if (first_free != -1) {
      int pid = first_free;
      uint8_t *p = fetch(pid);
      int32_t nf;
      memcpy(&nf, p, 4);
      first_free = nf;
      unpin(pid);
      return pid;
    }
    return (int)npages++;
  }

  void free_page(int pid) {
    uint8_t *p = fetch(pid);
    int32_t v = first_free;
    memcpy(p, &v, 4);
    mark_dirty(p);
    unpin(pid);
    first_free = pid;
  }

  void flush_all() {
    if (fp == nullptr) return;
    for (int s = 0; s < nslots; s++) {
      if (dirty_flag[s] && slot_page[s] != 0) {
        write_page(slot_page[s], buf + (size_t)s * PAGE_SZ);
      }
      dirty_flag[s] = 0;
    }
    uint8_t hdr[PAGE_SZ];
    memset(hdr, 0, sizeof(hdr));
    uint32_t m = HDR_MAGIC;
    int32_t r = root_pid, ff = first_free;
    uint32_t np = npages;
    memcpy(hdr, &m, 4);
    memcpy(hdr + 8, &r, 4);
    memcpy(hdr + 12, &np, 4);
    memcpy(hdr + 16, &ff, 4);
    write_page(0, hdr);
    fflush(fp);
  }

  void close_file() {
    if (fp != nullptr) {
      fclose(fp);
      fp = nullptr;
    }
  }

  uint8_t buf[(size_t)NCACHE * PAGE_SZ];

private:
  int slot_page[NCACHE];
  uint16_t pin_cnt[NCACHE];
  uint8_t dirty_flag[NCACHE];
  int prv[NCACHE], nxt[NCACHE];
  int head = -1, tail = -1;
  int nslots = 0;
  uint32_t npages = 1;
  int first_free = -1;
  std::unordered_map<int, int> mp;

  void detach(int s) {
    if (prv[s] != -1) nxt[prv[s]] = nxt[s];
    else head = nxt[s];
    if (nxt[s] != -1) prv[nxt[s]] = prv[s];
    else tail = prv[s];
    prv[s] = nxt[s] = -1;
  }

  void push_front(int s) {
    prv[s] = -1;
    nxt[s] = head;
    if (head != -1) prv[head] = s;
    head = s;
    if (tail == -1) tail = s;
  }

  void read_page(int pid, uint8_t *dst) {
    if (fp == nullptr) {
      memset(dst, 0, PAGE_SZ);
      return;
    }
    fseek(fp, (long)pid * PAGE_SZ, SEEK_SET);
    size_t got = fread(dst, 1, PAGE_SZ, fp);
    if (got < (size_t)PAGE_SZ) memset(dst + got, 0, PAGE_SZ - got);
  }

  void write_page(int pid, const uint8_t *src) {
    if (fp == nullptr) return;
    fseek(fp, (long)pid * PAGE_SZ, SEEK_SET);
    fwrite(src, 1, PAGE_SZ, fp);
  }
};

BufferPool bp;

// ---------------------------------------------------------------------------
// Tree operations
// ---------------------------------------------------------------------------

// First child index whose subtree may contain / follow e:
// child j where j = #{ keys[i] <= e }. Separator keys[i] is the minimum key
// of subtree children[i+1].
int choose_child(const uint8_t *p, const Entry &e) {
  int m = (int)icount(const_cast<uint8_t *>(p));
  int lo = 0, hi = m;
  while (lo < hi) {
    int mid = (lo + hi) >> 1;
    if (cmp_entry(e, *ikey(const_cast<uint8_t *>(p), mid)) < 0)
      hi = mid;
    else
      lo = mid + 1;
  }
  return lo;
}

int leaf_lower_bound(const uint8_t *p, const Entry &e) {
  int c = (int)lcount(const_cast<uint8_t *>(p));
  int lo = 0, hi = c;
  while (lo < hi) {
    int mid = (lo + hi) >> 1;
    if (cmp_entry(*lentry(const_cast<uint8_t *>(p), mid), e) < 0)
      lo = mid + 1;
    else
      hi = mid;
  }
  return lo;
}

// Minimum entry of a subtree; returns false if the subtree is an empty leaf.
bool subtree_min_into(int pid, Entry *out) {
  for (;;) {
    uint8_t *p = bp.fetch(pid);
    if (pg_type(p) == PG_LEAF) {
      if (lcount(p) == 0) {
        bp.unpin(pid);
        return false;
      }
      *out = *lentry(p, 0);
      bp.unpin(pid);
      return true;
    }
    int c = ichild(p, 0);
    bp.unpin(pid);
    pid = c;
  }
}

struct InsRes {
  bool split = false;
  int np = -1;
  Entry sep;
};

InsRes insert_rec(int pid, const Entry &e) {
  uint8_t *p = bp.fetch(pid);
  InsRes r;
  if (pg_type(p) == PG_LEAF) {
    int c = (int)lcount(p);
    int pos = leaf_lower_bound(p, e);
    if (pos < c && cmp_entry(*lentry(p, pos), e) == 0) {
      bp.unpin(pid); // duplicate (index,value): ignore
      return r;
    }
    if (c < MAX_L) {
      memmove(lentry(p, pos + 1), lentry(p, pos), (size_t)(c - pos) * sizeof(Entry));
      *lentry(p, pos) = e;
      lcount(p) = (uint32_t)(c + 1);
      bp.mark_dirty(p);
      bp.unpin(pid);
      return r;
    }
    // Leaf full: split.
    static Entry tmp[MAX_L + 1];
    for (int i = 0; i < pos; i++) tmp[i] = *lentry(p, i);
    tmp[pos] = e;
    for (int i = pos; i < c; i++) tmp[i + 1] = *lentry(p, i);
    int leftn = (MAX_L + 1) / 2;
    int rightn = MAX_L + 1 - leftn;
    for (int i = 0; i < leftn; i++) *lentry(p, i) = tmp[i];
    lcount(p) = (uint32_t)leftn;
    int npid = bp.alloc_page();
    uint8_t *q = bp.fetch(npid);
    *reinterpret_cast<uint32_t *>(q) = PG_LEAF;
    lcount(q) = (uint32_t)rightn;
    for (int i = 0; i < rightn; i++) *lentry(q, i) = tmp[leftn + i];
    int nx = lnext(p);
    lnext(q) = nx;
    lprev(q) = pid;
    lnext(p) = npid;
    if (nx != -1) {
      uint8_t *z = bp.fetch(nx);
      lprev(z) = npid;
      bp.mark_dirty(z);
      bp.unpin(nx);
    }
    bp.mark_dirty(p);
    bp.mark_dirty(q);
    r.split = true;
    r.np = npid;
    r.sep = *lentry(q, 0);
    bp.unpin(pid);
    bp.unpin(npid);
    return r;
  }

  // Internal node.
  int m = (int)icount(p);
  int j = choose_child(p, e);
  int cpid = ichild(p, j);
  InsRes cr = insert_rec(cpid, e);
  if (!cr.split) {
    bp.unpin(pid);
    return cr;
  }
  if (m < MAX_IK) {
    memmove(ikey(p, j + 1), ikey(p, j), (size_t)(m - j) * sizeof(Entry));
    memmove(&ichild(p, j + 2), &ichild(p, j + 1), (size_t)(m - j) * 4);
    *ikey(p, j) = cr.sep;
    ichild(p, j + 1) = cr.np;
    icount(p) = (uint32_t)(m + 1);
    bp.mark_dirty(p);
    bp.unpin(pid);
    return r;
  }
  // Internal node full: split.
  static Entry tk[MAX_IK + 1];
  static int32_t tc[MAX_IK + 2];
  for (int i = 0; i < j; i++) tk[i] = *ikey(p, i);
  tk[j] = cr.sep;
  for (int i = j; i < m; i++) tk[i + 1] = *ikey(p, i);
  for (int i = 0; i <= j; i++) tc[i] = ichild(p, i);
  tc[j + 1] = cr.np;
  for (int i = j + 1; i <= m; i++) tc[i + 1] = ichild(p, i);
  int total = m + 1;         // keys now
  int mid = total / 2;       // key pushed up
  Entry up = tk[mid];
  for (int i = 0; i < mid; i++) *ikey(p, i) = tk[i];
  for (int i = 0; i <= mid; i++) ichild(p, i) = tc[i];
  icount(p) = (uint32_t)mid;
  int npid = bp.alloc_page();
  uint8_t *q = bp.fetch(npid);
  *reinterpret_cast<uint32_t *>(q) = PG_INTERNAL;
  int rk = total - mid - 1;
  for (int i = 0; i < rk; i++) *ikey(q, i) = tk[mid + 1 + i];
  for (int i = 0; i <= rk; i++) ichild(q, i) = tc[mid + 1 + i];
  icount(q) = (uint32_t)rk;
  bp.mark_dirty(p);
  bp.mark_dirty(q);
  bp.unpin(pid);
  bp.unpin(npid);
  r.split = true;
  r.np = npid;
  r.sep = up;
  return r;
}

void do_insert(const Entry &e) {
  InsRes r = insert_rec(bp.root_pid, e);
  if (r.split) {
    int npid = bp.alloc_page();
    uint8_t *p = bp.fetch(npid);
    *reinterpret_cast<uint32_t *>(p) = PG_INTERNAL;
    icount(p) = 1;
    *ikey(p, 0) = r.sep;
    ichild(p, 0) = bp.root_pid;
    ichild(p, 1) = r.np;
    bp.mark_dirty(p);
    bp.unpin(npid);
    bp.root_pid = npid;
  }
}

// Repair child j of internal page p if it underflows after a deletion.
void fix_after_delete(uint8_t *p, int j) {
  int m = (int)icount(p);
  int cpid = ichild(p, j);
  uint8_t *c = bp.fetch(cpid);
  bool isleaf = pg_type(c) == PG_LEAF;
  int cmin = isleaf ? MIN_L : MIN_IK;
  int cc = (int)(isleaf ? lcount(c) : icount(c));
  if (cc >= cmin) {
    bp.unpin(cpid);
    return;
  }

  if (j > 0) {
    int lpid = ichild(p, j - 1);
    uint8_t *l = bp.fetch(lpid);
    int lc = (int)(isleaf ? lcount(l) : icount(l));
    if (lc > cmin) {
      // Borrow last entry/key from left sibling.
      if (isleaf) {
        memmove(lentry(c, 1), lentry(c, 0), (size_t)cc * sizeof(Entry));
        *lentry(c, 0) = *lentry(l, lc - 1);
        lcount(c) = (uint32_t)(cc + 1);
        lcount(l) = (uint32_t)(lc - 1);
        *ikey(p, j - 1) = *lentry(c, 0);
      } else {
        memmove(ikey(c, 1), ikey(c, 0), (size_t)cc * sizeof(Entry));
        memmove(&ichild(c, 1), &ichild(c, 0), (size_t)(cc + 1) * 4);
        *ikey(c, 0) = *ikey(p, j - 1);
        ichild(c, 0) = ichild(l, lc);
        icount(c) = (uint32_t)(cc + 1);
        *ikey(p, j - 1) = *ikey(l, lc - 1);
        icount(l) = (uint32_t)(lc - 1);
      }
      bp.mark_dirty(l);
      bp.mark_dirty(c);
      bp.unpin(lpid);
      bp.unpin(cpid);
      return;
    }
    // Merge c into left sibling.
    if (isleaf) {
      memcpy(lentry(l, lc), lentry(c, 0), (size_t)cc * sizeof(Entry));
      lcount(l) = (uint32_t)(lc + cc);
      int nx = lnext(c);
      lnext(l) = nx;
      if (nx != -1) {
        uint8_t *z = bp.fetch(nx);
        lprev(z) = lpid;
        bp.mark_dirty(z);
        bp.unpin(nx);
      }
    } else {
      *ikey(l, lc) = *ikey(p, j - 1);
      memcpy(ikey(l, lc + 1), ikey(c, 0), (size_t)cc * sizeof(Entry));
      memcpy(&ichild(l, lc + 1), &ichild(c, 0), (size_t)(cc + 1) * 4);
      icount(l) = (uint32_t)(lc + cc + 1);
    }
    memmove(ikey(p, j - 1), ikey(p, j), (size_t)(m - j) * sizeof(Entry));
    memmove(&ichild(p, j), &ichild(p, j + 1), (size_t)(m - j) * 4);
    icount(p) = (uint32_t)(m - 1);
    bp.mark_dirty(l);
    bp.unpin(lpid);
    bp.unpin(cpid);
    bp.free_page(cpid);
    return;
  }

  // j == 0: use right sibling.
  int rpid = ichild(p, 1);
  uint8_t *rr = bp.fetch(rpid);
  int rc = (int)(isleaf ? lcount(rr) : icount(rr));
  if (rc > cmin) {
    if (isleaf) {
      *lentry(c, cc) = *lentry(rr, 0);
      memmove(lentry(rr, 0), lentry(rr, 1), (size_t)(rc - 1) * sizeof(Entry));
      lcount(c) = (uint32_t)(cc + 1);
      lcount(rr) = (uint32_t)(rc - 1);
      *ikey(p, 0) = *lentry(rr, 0);
    } else {
      *ikey(c, cc) = *ikey(p, 0);
      ichild(c, cc + 1) = ichild(rr, 0);
      icount(c) = (uint32_t)(cc + 1);
      *ikey(p, 0) = *ikey(rr, 0);
      memmove(ikey(rr, 0), ikey(rr, 1), (size_t)(rc - 1) * sizeof(Entry));
      memmove(&ichild(rr, 0), &ichild(rr, 1), (size_t)rc * 4);
      icount(rr) = (uint32_t)(rc - 1);
    }
    bp.mark_dirty(c);
    bp.mark_dirty(rr);
    bp.unpin(cpid);
    bp.unpin(rpid);
    return;
  }
  // Merge right sibling into c.
  if (isleaf) {
    memcpy(lentry(c, cc), lentry(rr, 0), (size_t)rc * sizeof(Entry));
    lcount(c) = (uint32_t)(cc + rc);
    int nx = lnext(rr);
    lnext(c) = nx;
    if (nx != -1) {
      uint8_t *z = bp.fetch(nx);
      lprev(z) = cpid;
      bp.mark_dirty(z);
      bp.unpin(nx);
    }
  } else {
    *ikey(c, cc) = *ikey(p, 0);
    memcpy(ikey(c, cc + 1), ikey(rr, 0), (size_t)rc * sizeof(Entry));
    memcpy(&ichild(c, cc + 1), &ichild(rr, 0), (size_t)(rc + 1) * 4);
    icount(c) = (uint32_t)(cc + rc + 1);
  }
  memmove(ikey(p, 0), ikey(p, 1), (size_t)(m - 1) * sizeof(Entry));
  memmove(&ichild(p, 1), &ichild(p, 2), (size_t)(m - 1) * 4);
  icount(p) = (uint32_t)(m - 1);
  bp.mark_dirty(c);
  bp.unpin(cpid);
  bp.unpin(rpid);
  bp.free_page(rpid);
}

bool delete_rec(int pid, const Entry &e) {
  uint8_t *p = bp.fetch(pid);
  if (pg_type(p) == PG_LEAF) {
    int c = (int)lcount(p);
    int pos = leaf_lower_bound(p, e);
    if (pos >= c || cmp_entry(*lentry(p, pos), e) != 0) {
      bp.unpin(pid);
      return false;
    }
    memmove(lentry(p, pos), lentry(p, pos + 1), (size_t)(c - pos - 1) * sizeof(Entry));
    lcount(p) = (uint32_t)(c - 1);
    bp.mark_dirty(p);
    bp.unpin(pid);
    return true;
  }
  int j = choose_child(p, e);
  int cpid = ichild(p, j);
  bool found = delete_rec(cpid, e);
  if (!found) {
    bp.unpin(pid);
    return false;
  }
  // Deleting the minimum entry of a subtree stales the separator above it;
  // recompute it (an empty leaf gets merged away below, so skip it here).
  if (j >= 1) {
    Entry mn;
    if (subtree_min_into(cpid, &mn)) *ikey(p, j - 1) = mn;
  }
  fix_after_delete(p, j);
  bp.mark_dirty(p);
  bp.unpin(pid);
  return true;
}

void do_delete(const Entry &e) {
  bool found = delete_rec(bp.root_pid, e);
  if (!found) return;
  uint8_t *r = bp.fetch(bp.root_pid);
  if (pg_type(r) == PG_INTERNAL && icount(r) == 0) {
    int nr = ichild(r, 0);
    bp.unpin(bp.root_pid);
    bp.free_page(bp.root_pid);
    bp.root_pid = nr;
  } else {
    bp.unpin(bp.root_pid);
  }
}

// ---------------------------------------------------------------------------
// Fast IO
// ---------------------------------------------------------------------------
inline bool issp(int c) { return c == ' ' || c == '\n' || c == '\r' || c == '\t' || c == '\v' || c == '\f'; }

struct FastIn {
  static constexpr int SZ = 1 << 20;
  char buf[SZ];
  int pos = 0, len = 0;

  int gc() {
    if (pos >= len) {
      len = (int)fread(buf, 1, SZ, stdin);
      pos = 0;
      if (len <= 0) return -1;
    }
    return (unsigned char)buf[pos++];
  }

  int nextInt() {
    int c;
    do {
      c = gc();
    } while (c != -1 && !issp(c) && (c < '0' || c > '9') && c != '-');
    int sign = 1;
    if (c == '-') {
      sign = -1;
      c = gc();
    }
    long long v = 0;
    while (c >= '0' && c <= '9') {
      v = v * 10 + (c - '0');
      c = gc();
    }
    return (int)(sign * v);
  }

  int nextToken(char *out) {
    int c;
    do {
      c = gc();
    } while (c != -1 && issp(c));
    int n = 0;
    while (c != -1 && !issp(c)) {
      out[n++] = (char)c;
      c = gc();
    }
    return n;
  }
};

struct FastOut {
  static constexpr int SZ = 1 << 20;
  char buf[SZ];
  int pos = 0;

  void flush() {
    if (pos > 0) {
      fwrite(buf, 1, pos, stdout);
      pos = 0;
    }
  }
  void pc(char c) {
    if (pos == SZ) flush();
    buf[pos++] = c;
  }
  void ps(const char *s) {
    while (*s) pc(*s++);
  }
  void pi(int v) {
    if (v == 0) {
      pc('0');
      return;
    }
    unsigned u;
    if (v < 0) {
      pc('-');
      u = ~(unsigned)v + 1u;
    } else {
      u = (unsigned)v;
    }
    char t[12];
    int n = 0;
    while (u > 0) {
      t[n++] = (char)('0' + u % 10);
      u /= 10;
    }
    while (n > 0) pc(t[--n]);
  }
};

} // namespace

int main() {
  static FastIn in;
  static FastOut out;
  static char tok[128];

  bp.init("bpt_data.bin");

  int n = in.nextInt();
  for (int i = 0; i < n; i++) {
    int cl = in.nextToken(tok); // command
    if (cl == 0) break;
    if (tok[0] == 'i') { // insert idx val
      int il = in.nextToken(tok);
      int val = in.nextInt();
      Entry e;
      memset(&e, 0, sizeof(e));
      memcpy(e.key, tok, il);
      e.len = il;
      e.value = val;
      do_insert(e);
    } else if (tok[0] == 'd') { // delete idx val
      int il = in.nextToken(tok);
      int val = in.nextInt();
      Entry e;
      memset(&e, 0, sizeof(e));
      memcpy(e.key, tok, il);
      e.len = il;
      e.value = val;
      do_delete(e);
    } else { // find idx
      int il = in.nextToken(tok);
      Entry q;
      memset(&q, 0, sizeof(q));
      memcpy(q.key, tok, il);
      q.len = il;
      q.value = INT_MIN;
      int pid = bp.root_pid;
      uint8_t *p = bp.fetch(pid);
      while (pg_type(p) == PG_INTERNAL) {
        int j = choose_child(p, q);
        int cpid = ichild(p, j);
        bp.unpin(pid);
        pid = cpid;
        p = bp.fetch(pid);
      }
      int lo = leaf_lower_bound(p, q);
      bool any = false;
      for (;;) {
        int c = (int)lcount(p);
        bool stop = false;
        for (int k = lo; k < c; k++) {
          Entry *e = lentry(p, k);
          if (same_index(*e, tok, il)) {
            if (any) out.pc(' ');
            out.pi(e->value);
            any = true;
          } else {
            stop = true;
            break;
          }
        }
        int nx = lnext(p);
        bp.unpin(pid);
        if (stop || nx == -1) break;
        pid = nx;
        p = bp.fetch(pid);
        lo = 0;
      }
      if (!any) out.ps("null");
      out.pc('\n');
    }
  }

  out.flush();
  bp.flush_all();
  bp.close_file();
  return 0;
}
