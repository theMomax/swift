// RUN: %empty-directory(%t)
// RUN: %target-build-swift %S/Inputs/ExternalIndirect.swift -module-name ExternalIndirect -emit-module -emit-module-path %t/
// RUN: %target-build-swift %s -module-name Indirect -emit-module -emit-module-path %t/ -I %t
// RUN: %target-swift-symbolgraph-extract -module-name Indirect -I %t -pretty-print -output-dir %t
// RUN: %target-swift-symbolgraph-extract -module-name ExternalIndirect -I %t -pretty-print -output-dir %t
// RUN: %FileCheck %s --input-file %t/Indirect.symbols.json
// RUN: %FileCheck %s --input-file %t/ExternalIndirect.symbols.json --check-prefix EXTERNAL
// RUN: %FileCheck %s --input-file %t/Indirect@ExternalIndirect.symbols.json --check-prefix EXTENSION

public protocol P {
  func foo()
}

public protocol Q : P {}

public struct S : Q {
  public func foo() {}
}

// Q : P
// CHECK-DAG: "kind": "conformsTo",{{[[:space:]]*}}"source": "s:8Indirect1QP",{{[[:space:]]*}}"target": "s:8Indirect1PP"

// S : P
// CHECK-DAG: "kind": "conformsTo",{{[[:space:]]*}}"source": "s:8Indirect1SV",{{[[:space:]]*}}"target": "s:8Indirect1PP"

// S : Q
// CHECK-DAG: "kind": "conformsTo",{{[[:space:]]*}}"source": "s:8Indirect1SV",{{[[:space:]]*}}"target": "s:8Indirect1QP"

import ExternalIndirect

extension ES: EQ {
  public func foo() {}
}

// EQ : EP
// EXTERNAL-DAG: "kind": "conformsTo",{{[[:space:]]*}}"source": "s:16ExternalIndirect2EQP",{{[[:space:]]*}}"target": "s:16ExternalIndirect2EPP"

// ES : EP
// EXTENSION-DAG: "kind": "conformsTo",{{[[:space:]]*}}"source": "s:16ExternalIndirect2ESV",{{[[:space:]]*}}"target": "s:16ExternalIndirect2EPP"

// ES : EQ
// EXTENSION-DAG: "kind": "conformsTo",{{[[:space:]]*}}"source": "s:16ExternalIndirect2ESV",{{[[:space:]]*}}"target": "s:16ExternalIndirect2EQP"