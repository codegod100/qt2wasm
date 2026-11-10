;; A tiny component for the recipe test: calc.add(a, b) returns host.double(a + b).
(component
  (import "test:demo/host@1.0.0" (instance $host
    (export "double" (func (param "x" u32) (result u32)))))
  (core module $m
    (import "host" "double" (func $double (param i32) (result i32)))
    (func (export "add") (param i32 i32) (result i32)
      (call $double (i32.add (local.get 0) (local.get 1)))))
  (alias export $host "double" (func $double))
  (core func $double_lowered (canon lower (func $double)))
  (core instance $hi (export "double" (func $double_lowered)))
  (core instance $i (instantiate $m (with "host" (instance $hi))))
  (func $add (param "a" u32) (param "b" u32) (result u32) (canon lift (core func $i "add")))
  (instance $calc (export "add" (func $add)))
  (export "test:demo/calc@1.0.0" (instance $calc)))
