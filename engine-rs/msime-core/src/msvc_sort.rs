//! 照 MSVC 标准库写的 `std::partial_sort`。
//!
//! C++ 里 `partial_sort` 对相等元素的次序没有规定，实际次序由具体的堆算法决定；词格和句子解码都会遇到
//! 分数完全相同的假设，所以这里不用稳定排序，而是把同一套堆操作搬过来。基线是 MSVC 构建出的，
//! 对其他标准库实现，C++ 这一侧的平局次序可能不同。

pub fn partial_sort_by<T>(items: &mut Vec<T>, middle: usize, items_less: impl Fn(&T, &T) -> bool) {
    if middle == 0 || middle > items.len() {
        return;
    }
    let mut order: Vec<usize> = (0..items.len()).collect();
    let less = |a: usize, b: usize| items_less(&items[a], &items[b]);

    // 把空位下沉到叶子再把值上浮（MSVC 的 `_Pop_heap_hole_by_index`）。
    fn pop_heap_hole(order: &mut [usize], mut hole: usize, bottom: usize, value: usize, less: &impl Fn(usize, usize) -> bool) {
        let top = hole;
        let mut index = hole;
        let max_non_leaf = (bottom - 1) >> 1;
        while index < max_non_leaf {
            index = 2 * index + 2;
            if less(order[index], order[index - 1]) {
                index -= 1;
            }
            order[hole] = order[index];
            hole = index;
        }
        if index == max_non_leaf && bottom % 2 == 0 {
            order[hole] = order[bottom - 1];
            hole = bottom - 1;
        }
        while top < hole {
            let parent = (hole - 1) >> 1;
            if !less(order[parent], value) {
                break;
            }
            order[hole] = order[parent];
            hole = parent;
        }
        order[hole] = value;
    }

    // make_heap
    let mut hole = middle >> 1;
    while hole > 0 {
        hole -= 1;
        let value = order[hole];
        pop_heap_hole(&mut order, hole, middle, value, &less);
    }
    // 其余元素：比堆顶小就换进堆
    for next in middle..order.len() {
        if less(order[next], order[0]) {
            let value = order[next];
            order[next] = order[0];
            pop_heap_hole(&mut order, 0, middle, value, &less);
        }
    }
    // sort_heap
    let mut last = middle;
    while last >= 2 {
        last -= 1;
        let value = order[last];
        order[last] = order[0];
        pop_heap_hole(&mut order, 0, last, value, &less);
    }

    let mut slots: Vec<Option<T>> = items.drain(..).map(Some).collect();
    items.extend(order.into_iter().map(|i| slots[i].take().expect("每个下标只用一次")));
}

