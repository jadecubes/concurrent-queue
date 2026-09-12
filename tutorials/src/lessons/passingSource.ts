import implementation from '../../../include/cq/mutex_queue.ipp?raw'
import header from '../../../include/cq/mutex_queue.hpp?raw'
// Source is bundled directly from the library, never duplicated in tutorials.
export { implementation, header }
export function slice(source: string, start: string, end: string): string {
  const a = source.indexOf(start), b = source.indexOf(end, a + start.length)
  if (a < 0 || b < 0) throw new Error(`Queue source slice drifted: ${start} / ${end}`)
  return source.slice(a, b).trim()
}
export const sourceCode = {
  push: slice(implementation, 'template <typename T>\nbool MutexQueue<T>::push', 'template <typename T>\nbool MutexQueue<T>::try_push'),
  tryPush: slice(implementation, 'template <typename T>\nbool MutexQueue<T>::try_push', 'template <typename T>\nbool MutexQueue<T>::pop'),
  pop: slice(implementation, 'template <typename T>\nbool MutexQueue<T>::pop', 'template <typename T>\nbool MutexQueue<T>::try_pop'),
  tryPop: slice(implementation, 'template <typename T>\nbool MutexQueue<T>::try_pop', 'template <typename T>\nvoid MutexQueue<T>::close'),
  close: slice(implementation, 'template <typename T>\nvoid MutexQueue<T>::close', 'template <typename T>\nbool MutexQueue<T>::closed'),
  snapshots: slice(implementation, 'template <typename T>\nbool MutexQueue<T>::closed', 'template <typename T>\nvoid MutexQueue<T>::enqueue_locked'),
  ring: slice(implementation, 'template <typename T>\nvoid MutexQueue<T>::enqueue_locked', '}  // namespace cq'),
}
export const ownershipContract = slice(header, '  /// Enqueues a value, blocking', '  /// Dequeues into out, blocking')
export const lifetimeContract = slice(header, '/// Lifetime:', '/// Exceptions:')
export const snapshotContract = slice(header, '  /// @return true once close()', ' private:')
export const taskCode = (task: number) => task === 2 ? sourceCode.tryPush : task === 4 ? sourceCode.close + '\n\n' + sourceCode.pop : task === 5 ? sourceCode.snapshots + '\n\n' + sourceCode.push : task === 3 ? sourceCode.close + '\n\n' + sourceCode.push + '\n\n' + sourceCode.pop : sourceCode.push + '\n\n' + sourceCode.pop
