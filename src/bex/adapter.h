#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <functional>
#include <string>
#include <string_view>
#include <vector>
#include <unordered_map>

#include "../common/adapter.h"

#include <bex.h>

// BEX_MODE selects how bex is invoked from the bdd-benchmark adapter:
//   "bdd"  - call bex_bdd_* directly (bottom-up construction). DEFAULT.
//   "sub"  - build an AST via bex_ast_*, then bex_subsolve to BDD.
//   "swap" - build an AST via bex_ast_*, then bex_swapsolve to BDD.
enum class bex_mode { BDD, SUB, SWAP };
inline bex_mode get_bex_mode() {
  const char* m = std::getenv("BEX_MODE");
  if (!m) return bex_mode::BDD;
  std::string s(m);
  if (s == "sub")  return bex_mode::SUB;
  if (s == "swap") return bex_mode::SWAP;
  return bex_mode::BDD;
}

class bex_bdd_adapter;

// Global adapter pointer for operators (definition)
thread_local bex_bdd_adapter* g_bex_adapter = nullptr;

class bex_bdd_adapter
{
public:
  static constexpr std::string_view name = "Bex";
  static constexpr std::string_view dd   = "BDD";

  static constexpr bool needs_extend     = false;
  static constexpr bool needs_frame_rule = true;

  static constexpr bool complement_edges = false;

  using dd_t         = bex_nid_t;
  using build_node_t = bex_nid_t;

private:
  const int _varcount;
  bex_ast_t* _ast;
  bex_bdd_t* _bdd;
  bex_swap_t* _swap;
  bex_nid_t _latest_build;
  bex_mode _mode;
  mutable std::unordered_map<uint64_t, bex_nid_t> _ast_to_bdd_cache;

  // Init and Deinit
public:
  bex_bdd_adapter(int varcount)
    : _varcount(varcount)
    , _ast(bex_ast_new())
    , _bdd(bex_bdd_new())
    , _swap(bex_swap_new())
    , _mode(get_bex_mode())
  {
    // Direct single-threaded ITE recursion is much faster than swarm
    // dispatch for bottom-up bdd-benchmark workloads.
    bex_bdd_set_direct_ite(_bdd, true);
  }

  ~bex_bdd_adapter() {
    bex_ast_free(_ast);
    bex_bdd_free(_bdd);
    bex_swap_free(_swap);
  }

  template <typename F>
  int
  run(const F& f)
  {
    g_bex_adapter = this;
    int result = f();
    g_bex_adapter = nullptr;
    return result;
  }

  // BDD Operations
  inline bex_nid_t
  top()
  {
    return bex_top();
  }

  inline bex_nid_t
  bot()
  {
    return bex_bot();
  }

  inline bex_nid_t
  ithvar(uint32_t label)
  {
    bex_vid_t vid = { label };
    return bex_ithvar(vid);
  }

  inline bex_nid_t
  nithvar(uint32_t label)
  {
    bex_vid_t vid = { label };
    return bex_not(bex_ithvar(vid));
  }

  template <typename IT>
  inline bex_nid_t
  cube(IT rbegin, IT rend)
  {
    bex_nid_t res = top();
    while (rbegin != rend) { res = bex_ast_and(_ast, res, ithvar(*(rbegin++))); }
    return res;
  }

  inline bex_nid_t
  cube(const std::function<bool(int)>& pred)
  {
    bex_nid_t res = top();
    for (int i = _varcount - 1; 0 <= i; --i) {
      if (pred(i)) { res = bex_ast_and(_ast, res, ithvar(i)); }
    }
    return res;
  }

  inline bex_nid_t
  apply_and(const bex_nid_t& f, const bex_nid_t& g)
  {
    return (_mode == bex_mode::BDD) ? bex_bdd_and(_bdd, f, g)
                                    : bex_ast_and(_ast, f, g);
  }

  inline bex_nid_t
  apply_or(const bex_nid_t& f, const bex_nid_t& g)
  {
    return (_mode == bex_mode::BDD) ? bex_bdd_or(_bdd, f, g)
                                    : bex_ast_or(_ast, f, g);
  }

  inline bex_nid_t
  apply_xor(const bex_nid_t& f, const bex_nid_t& g)
  {
    return (_mode == bex_mode::BDD) ? bex_bdd_xor(_bdd, f, g)
                                    : bex_ast_xor(_ast, f, g);
  }

  inline bex_nid_t
  apply_not(const bex_nid_t& f)
  {
    return bex_not(f);
  }

  inline bex_nid_t
  apply_imp(const bex_nid_t& f, const bex_nid_t& g)
  {
    // IMPLICATION: f -> g is equivalent to ~f | g
    return apply_or(apply_not(f), g);
  }

  inline bex_nid_t
  apply_diff(const bex_nid_t& f, const bex_nid_t& g)
  {
    // DIFFERENCE: f & ~g
    return apply_and(f, apply_not(g));
  }

  inline bex_nid_t
  apply_xnor(const bex_nid_t& f, const bex_nid_t& g)
  {
    // XNOR: ~(f XOR g) = NOT XOR
    return apply_not(apply_xor(f, g));
  }

  // In AST modes, convert the AST nid to a BDD nid using the configured
  // solver (subsolve or swapsolve). SWAP mode produces a nid in the swap
  // solver's own storage, so we copy the result into _bdd afterward.
  // Results are cached so repeated nodecount/satcount calls don't re-solve.
  inline bex_nid_t _as_bdd(const bex_nid_t& f) {
    if (_mode == bex_mode::BDD) return f;
    if (bex_is_lit(f)) return f;
    auto it = _ast_to_bdd_cache.find(f.nid);
    if (it != _ast_to_bdd_cache.end()) return it->second;
    bex_nid_t bdd_nid;
    if (_mode == bex_mode::SWAP) {
      bex_nid_t swap_nid = bex_swapsolve(_ast, _swap, f);
      bdd_nid = bex_swap_copy_to_bdd(_swap, _bdd, swap_nid);
    } else {
      bdd_nid = bex_subsolve(_ast, _bdd, f);
    }
    _ast_to_bdd_cache[f.nid] = bdd_nid;
    return bdd_nid;
  }

  inline uint64_t
  nodecount(const bex_nid_t& f)
  {
    if (f.nid == bex_top().nid) return 1;
    if (f.nid == bex_bot().nid) return 1;
    return bex_bdd_node_count(_bdd, _as_bdd(f));
  }

  inline uint64_t
  satcount(const bex_nid_t& f)
  {
    if (f.nid == bex_top().nid) return 1;
    if (f.nid == bex_bot().nid) return 0;
    return bex_bdd_solution_count(_bdd, _as_bdd(f));
  }

  inline uint64_t
  satcount(const bex_nid_t& f, const size_t vc)
  {
    if (f.nid == bex_top().nid) return (1ULL << vc);
    if (f.nid == bex_bot().nid) return 0;
    return bex_bdd_solution_count(_bdd, _as_bdd(f));
  }

  inline size_t
  allocated_nodes()
  {
    // TODO: implement total allocated nodes count
    // For now return 0 as requested
    return 0;
  }

  void
  print_dot(const bex_nid_t&, const std::string&)
  {
    std::cerr << "bex_bdd_adapter does not support dot export" << std::endl;
  }

  void
  save(const bex_nid_t& f, const std::string& path)
  {
    // TODO: implement this in the FFI layer
  }

  bex_nid_t
  load(const std::string& path)
  {
    // TODO: implement this in the FFI layer
    return bex_nid_t();
  }

  // BDD Build Operations
public:
  inline bex_nid_t
  build_node(const bool value)
  {
    const bex_nid_t res = value ? top() : bot();
    if (_latest_build.nid == 0 || _latest_build.nid == top().nid || _latest_build.nid == bot().nid) {
      _latest_build = res;
    }
    return res;
  }

  inline bex_nid_t
  build_node(const uint32_t label,
             const bex_nid_t& low,
             const bex_nid_t& high)
  {
    return _latest_build = (_mode == bex_mode::BDD)
      ? bex_bdd_ite(_bdd, ithvar(label), high, low)
      : bex_ast_ite(_ast, ithvar(label), high, low);
  }

  inline bex_nid_t
  build()
  {
    return _latest_build;
  }

  inline bex_nid_t
  apply_ite(const bex_nid_t& i, const bex_nid_t& t, const bex_nid_t& e)
  {
    return (_mode == bex_mode::BDD) ? bex_bdd_ite(_bdd, i, t, e)
                                    : bex_ast_ite(_ast, i, t, e);
  }

  // Conversion methods
  inline bex_nid_t
  ast_to_bdd_subsolver(const bex_nid_t& n)
  {
    return bex_subsolve(_ast, _bdd, n);
  }

  inline bex_nid_t
  ast_to_bdd_swapsolver(const bex_nid_t& n)
  {
    return bex_swapsolve(_ast, _swap, n);
  }

private:
  inline bex_nid_t
  get_or_convert_to_bdd(const bex_nid_t& ast_nid) const
  {
    // If it's a literal (constant or variable), return directly
    if (bex_is_lit(ast_nid)) {
      return ast_nid;
    }

    // Validate AST index before conversion
    if (bex_is_ast(ast_nid)) {
      size_t ast_len = bex_ast_len(_ast);
      size_t node_idx = ast_nid.nid & 0xFFFFFFFF; // Extract lower 32 bits (IDX_MASK)
      if (node_idx >= ast_len) {
        // Invalid AST index - return bottom to avoid crash
        return bex_bot();
      }
    }

    auto it = _ast_to_bdd_cache.find(ast_nid.nid);
    if (it != _ast_to_bdd_cache.end()) {
      return it->second;
    }

    bex_nid_t bdd_nid = bex_subsolve(_ast, _bdd, ast_nid);

    // Validate the result before caching and returning
    if (bex_is_ast(bdd_nid)) {
      // swapsolve should not return AST nodes
      return bex_bot();
    }

    _ast_to_bdd_cache[ast_nid.nid] = bdd_nid;
    return bdd_nid;
  }

public:

  template<typename F>
  inline bex_nid_t
  exists(const bex_nid_t& f, F&& predicate)
  {
    // TODO: implement existential quantification - complex operation
    // For now return O (false) as requested
    return bot();
  }

  inline bex_nid_t
  relnext(const bex_nid_t& f, const bex_nid_t& relation, const bex_nid_t& support)
  {
    // TODO: implement relational product - complex operation
    // For now return O (false) as requested
    return bot();
  }

  inline bex_nid_t
  relprev(const bex_nid_t& f, const bex_nid_t& relation, const bex_nid_t& support)
  {
    // TODO: implement relational product - complex operation
    // For now return O (false) as requested
    return bot();
  }

  template<typename T>
  inline bex_nid_t
  satone(const bex_nid_t& f, T& var_cube)
  {
    // TODO: implement satisfying assignment extraction
    // For now return O (false) as requested
    return bot();
  }

  inline bex_nid_t
  ite(const bex_nid_t& i, const bex_nid_t& t, const bex_nid_t& e)
  {
    return apply_ite(i, t, e);
  }

  template<typename F>
  inline bex_nid_t
  forall(const bex_nid_t& f, F&& predicate)
  {
    // TODO: implement universal quantification - complex operation
    // For now return O (false) as requested
    return bot();
  }

  inline auto
  pickcube(const bex_nid_t& f)
  {
    // TODO: implement cube extraction
    // For now return empty vector of pairs as requested
    return std::vector<std::pair<int, bool>>{};
  }
};

// Global adapter pointer for operators (not thread-safe, but needed for template compatibility)
extern thread_local bex_bdd_adapter* g_bex_adapter;

// Operators for bex_nid_t to work with template code
inline bex_nid_t operator&(const bex_nid_t& lhs, const bex_nid_t& rhs) {
  return g_bex_adapter ? g_bex_adapter->apply_and(lhs, rhs) : lhs;
}

inline bex_nid_t operator|(const bex_nid_t& lhs, const bex_nid_t& rhs) {
  return g_bex_adapter ? g_bex_adapter->apply_or(lhs, rhs) : lhs;
}

inline bex_nid_t operator~(const bex_nid_t& f) {
  return g_bex_adapter ? g_bex_adapter->apply_not(f) : f;
}

inline bex_nid_t& operator&=(bex_nid_t& lhs, const bex_nid_t& rhs) {
  if (g_bex_adapter) lhs = g_bex_adapter->apply_and(lhs, rhs);
  return lhs;
}

inline bex_nid_t& operator|=(bex_nid_t& lhs, const bex_nid_t& rhs) {
  if (g_bex_adapter) lhs = g_bex_adapter->apply_or(lhs, rhs);
  return lhs;
}

inline bool operator==(const bex_nid_t& lhs, const bex_nid_t& rhs) {
  return lhs.nid == rhs.nid;
}

inline bool operator!=(const bex_nid_t& lhs, const bex_nid_t& rhs) {
  return lhs.nid != rhs.nid;
}

inline bex_nid_t operator-(const bex_nid_t& lhs, const bex_nid_t& rhs) {
  return g_bex_adapter ? g_bex_adapter->apply_diff(lhs, rhs) : lhs;
}

inline bex_nid_t operator^(const bex_nid_t& lhs, const bex_nid_t& rhs) {
  return g_bex_adapter ? g_bex_adapter->apply_xor(lhs, rhs) : lhs;
}
