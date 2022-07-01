// RUN: %empty-directory(%t)
// RUN: %target-build-swift %S/Inputs/ExternalNames.swift -module-name ExternalNames -emit-module -emit-module-path %t/
// RUN: %target-build-swift %s -module-name Names -emit-module -emit-module-path %t/ -I %t
// RUN: %target-swift-symbolgraph-extract -module-name Names -I %t -pretty-print -output-dir %t -emit-extension-block-symbols
// RUN: %FileCheck %s --input-file %t/Names.symbols.json
// RUN: %FileCheck %s --input-file %t/Names.symbols.json --check-prefix=FUNC
// RUN: %FileCheck %s --input-file %t/Names.symbols.json --check-prefix=INNERTYPE
// RUN: %FileCheck %s --input-file %t/Names.symbols.json --check-prefix=INNERTYPEALIAS
// RUN: %FileCheck %s --input-file %t/Names.symbols.json --check-prefix=INNERENUM
// RUN: %FileCheck %s --input-file %t/Names.symbols.json --check-prefix=INNERCASE

// RUN: %FileCheck %s --input-file %t/Names@ExternalNames.symbols.json --check-prefix=EXT_INNERTYPE
// RUN: %FileCheck %s --input-file %t/Names@ExternalNames.symbols.json --check-prefix=EXT_INNERENUM

public struct MyStruct {
  public struct InnerStruct {}

  public typealias InnerTypeAlias = InnerStruct

  public func foo() {}

  public enum InnerEnum {
      case InnerCase
  }
}

// CHECK-LABEL: "precise": "s:5Names8MyStructV"
// CHECK: names
// CHECK-NEXT: "title": "MyStruct"

// FUNC-LABEL: "precise": "s:5Names8MyStructV3fooyyF",
// FUNC: names
// FUNC-NEXT: "title": "foo()"

// INNERTYPE-LABEL: "precise": "s:5Names8MyStructV05InnerC0V"
// INNERTYPE: names
// INNERTYPE-NEXT: "title": "MyStruct.InnerStruct"

// INNERTYPEALIAS-LABEL: "precise": "s:5Names8MyStructV14InnerTypeAliasa"
// INNERTYPEALIAS: names
// INNERTYPEALIAS-NEXT: "title": "MyStruct.InnerTypeAlias"

// INNERENUM-LABEL: "precise": "s:5Names8MyStructV9InnerEnumO",
// INNERENUM: names
// INNERENUM-NEXT: "title": "MyStruct.InnerEnum"

// INNERCASE-LABEL: "precise": "s:5Names8MyStructV9InnerEnumO0D4CaseyA2EmF",
// INNERCASE: names
// INNERCASE-NEXT: "title": "MyStruct.InnerEnum.InnerCase",


import ExternalNames

public extension ExternalStruct.InnerStruct {
  func foo() {}
}

public extension ExternalStruct.InnerEnum {
  func foo() {}
}

// EXT_INNERTYPE-LABEL: "precise": "s:e:s:13ExternalNames0A6StructV05InnerC0V0B0E3fooyyF"
// EXT_INNERTYPE: names
// EXT_INNERTYPE-NEXT: "title": "ExternalStruct.InnerStruct"

// EXT_INNERENUM-LABEL: "precise": "s:e:s:13ExternalNames0A6StructV9InnerEnumO0B0E3fooyyF",
// EXT_INNERENUM: names
// EXT_INNERENUM-NEXT: "title": "ExternalStruct.InnerEnum"