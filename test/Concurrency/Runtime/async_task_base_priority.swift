// RUN: %target-run-simple-swift( -Xfrontend -disable-availability-checking %import-libdispatch -parse-as-library) | %FileCheck --dump-input=always %s

// REQUIRES: executable_test
// REQUIRES: concurrency
// REQUIRES: libdispatch

// rdar://76038845
// REQUIRES: concurrency_runtime
// UNSUPPORTED: back_deployment_runtime

import Dispatch

// Work around the inability of older Swift runtimes to print a task priority.
extension TaskPriority: CustomStringConvertible {
  public var description: String {
    "TaskPriority(rawValue: \(rawValue))"
  }
}

@available(SwiftStdlib 5.1, *)
@main struct Main {
  static func main() async {
    print("main priority: \(Task.currentPriority)") // CHECK: main priority: TaskPriority(rawValue: [[#MAIN_PRIORITY:]])

    await test_structured_concurrency_base_priority_propagation()
    await test_unstructured_concurrency_base_priority_propagation()
  }
}

func test_structured_concurrency_base_priority_propagation() async {
  @Sendable
  func loopUntil(priority: TaskPriority) async {
    while (Task.currentPriority != priority) {
      await Task.sleep(1_000_000_000)
    }
  }

  @Sendable
  func getNestedTaskPriority() async -> (TaskPriority, TaskPriority) {
    return (Task.basePriority!, Task.currentPriority)
  }

  await Task(priority: .background) {
    print ("Testing structured concurrency base priority propagation");
    await loopUntil(priority: .default)

    let base_pri = Task.basePriority! // QoS BG
    print("base_pri: \(base_pri)") // CHECK: base_pri: TaskPriority(rawValue: 9)
    let cur_pri = Task.currentPriority // QoS DEF
    print("cur_pri: \(cur_pri)") // CHECK: cur_pri: TaskPriority(rawValue: [[#MAIN_PRIORITY:]])

    // Structured concurrency via async let
    async let (nestedBasePri, nestedCurPri) = getNestedTaskPriority()

    print("Nested base_pri == base_pri: \((await nestedBasePri) == base_pri)") // CHECK: Nested base_pri == base_pri: true
    print("Nested cur_pri == cur_pri: \((await nestedCurPri) == cur_pri)") // CHECK: Nested cur_pri == cur_pri: true

    // Structured concurrency via task groups
    await withTaskGroup(of: (TaskPriority, TaskPriority).self, returning: Void.self) { group in
      group.addTask {
          return await getNestedTaskPriority()
      }
      let (child1_base_pri, child1_cur_pri) = await group.next()!
      print("child1_base_pri == base_pri: \(child1_base_pri == base_pri)") // CHECK: child1_base_pri == base_pri: true
      print("child1_cur_pri == cur_pri: \(child1_cur_pri == cur_pri)") // CHECK: child1_cur_pri == cur_pri: true

      group.addTask(priority: .utility) {
          await loopUntil(priority: .default)
          return await getNestedTaskPriority()
      }
      let (child2_base_pri, child2_cur_pri) = await group.next()!
      print("child2_base_pri = .utility : \(child2_base_pri == .utility)") // CHECK: child2_base_pri = .utility : true
      print("child2_cur_pri == cur_pri: \(child2_cur_pri == cur_pri)") // CHECK: child2_cur_pri == cur_pri: true
    }
  }.get()

}

func test_unstructured_concurrency_base_priority_propagation() async {
  @Sendable
  func loopUntil(priority: TaskPriority) async {
    while (Task.currentPriority != priority) {
      await Task.sleep(1_000_000_000)
    }
  }

  @Sendable
  func getNestedTaskPriority() -> (TaskPriority, TaskPriority) {
    return (Task.basePriority!, Task.currentPriority)
  }

  await Task(priority: .background) {
    print ("Testing unstructured concurrency base priority propagation");
    await loopUntil(priority: .default)

    let base_pri = Task.basePriority! // QoS BG
    print("base_pri: \(base_pri)") // CHECK: base_pri: TaskPriority(rawValue: 9)
    let cur_pri = Task.currentPriority // QoS DEF
    print("cur_pri: \(cur_pri)") // CHECK: cur_pri: TaskPriority(rawValue: [[#MAIN_PRIORITY:]])

    // Create some unstructured tasks
    let child1Task  = Task {
        return getNestedTaskPriority()
    }

    let child2Task  = Task(priority: .utility) {
        return getNestedTaskPriority()
    }

    await Task.sleep(1_000_000_000)

    let (child1_base_pri, _) = await child1Task.get()
    let (child2_base_pri, _) = await child2Task.get()

    print("child1_base_pri == base_pri: \(child1_base_pri == base_pri)") // CHECK: child1_base_pri == base_pri: true
    print("child2_base_pri == utility: \(child2_base_pri == .utility)") // CHECK: child2_base_pri == utility: true
  }.get()
}
