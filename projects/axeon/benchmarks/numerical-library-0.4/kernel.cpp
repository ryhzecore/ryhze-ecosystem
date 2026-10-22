// Inline only the adapter target so this C++-only bridge adds no second call.
// The driver still cannot inline the opaque library entry point (separate TU).
namespace reference {
class Tensor;
struct Budget;
class View;
[[gnu::always_inline]] inline Tensor matmul(Budget&, const View&, const View&);
}
#include "kernel.hpp"
reference::Tensor checked_matmul(reference::Budget& budget, const reference::View& left,
                                 const reference::View& right) {
    return reference::matmul(budget, left, right);
}
