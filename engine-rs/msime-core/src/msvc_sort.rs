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


// ---------------------------------------------------------------------------------------------
// std::sort（内省排序）。同样照 MSVC 的 `<algorithm>` 搬：32 以内插入排序，否则按三数取中/九数取中分区，
// 划分次数超过 1.5·log2(N) 时退成堆排序。相等元素的最终次序由这套交换决定。
// ---------------------------------------------------------------------------------------------

const ISORT_MAX: usize = 32;

type Less<'a> = &'a dyn Fn(usize, usize) -> bool;

fn insertion_sort(v: &mut [usize], first: usize, last: usize, less: Less) {
    if first == last {
        return;
    }
    let mut mid = first;
    loop {
        mid += 1;
        if mid == last {
            break;
        }
        let value = v[mid];
        if less(value, v[first]) {
            // 新的最小元素：整段后移一位，放到最前。
            let mut i = mid;
            while i > first {
                v[i] = v[i - 1];
                i -= 1;
            }
            v[first] = value;
        } else {
            let mut hole = mid;
            let mut prev = hole;
            loop {
                prev -= 1;
                if !less(value, v[prev]) {
                    break;
                }
                v[hole] = v[prev];
                hole = prev;
            }
            v[hole] = value;
        }
    }
}

fn med3(v: &mut [usize], first: usize, mid: usize, last: usize, less: Less) {
    if less(v[mid], v[first]) {
        v.swap(mid, first);
    }
    if less(v[last], v[mid]) {
        v.swap(last, mid);
        if less(v[mid], v[first]) {
            v.swap(mid, first);
        }
    }
}

/// `last` 是最后一个元素的下标（闭区间），和 MSVC 的 `_Guess_median_unchecked` 一致。
fn guess_median(v: &mut [usize], first: usize, mid: usize, last: usize, less: Less) {
    let count = last - first;
    if 40 < count {
        let step = (count + 1) >> 3;
        let two_step = step << 1;
        med3(v, first, first + step, first + two_step, less);
        med3(v, mid - step, mid, mid + step, less);
        med3(v, last - two_step, last - step, last, less);
        med3(v, first + step, mid, last - step, less);
    } else {
        med3(v, first, mid, last, less);
    }
}

fn partition_by_pivot(v: &mut [usize], first: usize, mut pfirst: usize, last: usize, less: Less) -> (usize, usize) {
    let mut plast = pfirst + 1;
    while first < pfirst && !less(v[pfirst - 1], v[pfirst]) && !less(v[pfirst], v[pfirst - 1]) {
        pfirst -= 1;
    }
    while plast < last && !less(v[plast], v[pfirst]) && !less(v[pfirst], v[plast]) {
        plast += 1;
    }
    let mut gfirst = plast;
    let mut glast = pfirst;
    loop {
        while gfirst < last {
            if less(v[pfirst], v[gfirst]) {
                gfirst += 1;
                continue;
            } else if less(v[gfirst], v[pfirst]) {
                break;
            } else if plast != gfirst {
                v.swap(plast, gfirst);
                plast += 1;
            } else {
                plast += 1;
            }
            gfirst += 1;
        }
        while first < glast {
            let prev = glast - 1;
            if less(v[prev], v[pfirst]) {
                glast -= 1;
                continue;
            } else if less(v[pfirst], v[prev]) {
                break;
            } else {
                pfirst -= 1;
                if pfirst != prev {
                    v.swap(pfirst, prev);
                }
            }
            glast -= 1;
        }
        if glast == first && gfirst == last {
            return (pfirst, plast);
        }
        if glast == first {
            // 下面没有空位：把枢轴往上转。
            if plast != gfirst {
                v.swap(pfirst, plast);
            }
            plast += 1;
            v.swap(pfirst, gfirst);
            pfirst += 1;
            gfirst += 1;
        } else if gfirst == last {
            // 上面没有空位：把枢轴往下转。
            glast -= 1;
            pfirst -= 1;
            if glast != pfirst {
                v.swap(glast, pfirst);
            }
            plast -= 1;
            v.swap(pfirst, plast);
        } else {
            glast -= 1;
            v.swap(gfirst, glast);
            gfirst += 1;
        }
    }
}

fn sort_unchecked(v: &mut [usize], mut first: usize, mut last: usize, mut ideal: isize, less: Less) {
    loop {
        if last - first <= ISORT_MAX {
            insertion_sort(v, first, last, less);
            return;
        }
        if ideal <= 0 {
            // 划分太多次：堆排序（make_heap + sort_heap，就是 middle = 长度的 partial_sort）。
            let mut part: Vec<usize> = v[first..last].to_vec();
            let middle = part.len();
            partial_sort_by(&mut part, middle, |a, b| less(*a, *b));
            v[first..last].copy_from_slice(&part);
            return;
        }
        let mid = first + ((last - first) >> 1);
        guess_median(v, first, mid, last - 1, less);
        let (mid_first, mid_second) = partition_by_pivot(v, first, mid, last, less);
        ideal = (ideal >> 1) + (ideal >> 2);
        if mid_first - first < last - mid_second {
            sort_unchecked(v, first, mid_first, ideal, less);
            first = mid_second;
        } else {
            sort_unchecked(v, mid_second, last, ideal, less);
            last = mid_first;
        }
    }
}

/// MSVC 的 `std::sort(begin, end, items_less)`。
pub fn sort_by<T>(items: &mut Vec<T>, items_less: impl Fn(&T, &T) -> bool) {
    let n = items.len();
    if n < 2 {
        return;
    }
    let mut order: Vec<usize> = (0..n).collect();
    {
        let less = |a: usize, b: usize| items_less(&items[a], &items[b]);
        sort_unchecked(&mut order, 0, n, n as isize, &less);
    }
    let mut slots: Vec<Option<T>> = items.drain(..).map(Some).collect();
    items.extend(order.into_iter().map(|i| slots[i].take().expect("每个下标只用一次")));
}

#[cfg(test)]
mod sort_tests {
    use super::sort_by;

    #[test]
    fn sorts_and_keeps_every_element() {
        for n in [0usize, 1, 2, 5, 31, 32, 33, 100, 500, 3000] {
            let mut values: Vec<(u32, usize)> = (0..n).map(|i| (((i * 7919 + 13) % 17) as u32, i)).collect();
            sort_by(&mut values, |a, b| a.0 > b.0);
            assert!(values.windows(2).all(|w| w[0].0 >= w[1].0), "n = {n}");
            let mut ids: Vec<usize> = values.iter().map(|v| v.1).collect();
            ids.sort();
            assert_eq!(ids, (0..n).collect::<Vec<_>>());
        }
    }
}
