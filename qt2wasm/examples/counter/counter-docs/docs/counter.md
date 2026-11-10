# <a id="component"></a>demo:counter@0.1.0 API reference

 - Imports:
    - interface `demo:counter/counter-events@0.1.0`
    - interface `demo:counter/types@0.1.0`
 - Exports:
    - interface `demo:counter/counter@0.1.0`

## <a id="demo_counter_counter_events_0_1_0"></a>Import interface demo:counter/counter-events@0.1.0

Signals of Counter, delivered to the host as calls.

----

### Functions

#### <a id="value_changed"></a>`value-changed: func`

Emitted when the count changes to `value`.

##### Params

- <a id="value_changed.source"></a>`source`: `u32`
- <a id="value_changed.value"></a>`value`: `s32`

#### <a id="overflowed"></a>`overflowed: func`

Emitted instead of stepping past `INT_MAX`: `why` says why and `at` is
the value the step would have reached.

##### Params

- <a id="overflowed.source"></a>`source`: `u32`
- <a id="overflowed.why"></a>`why`: `string`
- <a id="overflowed.at"></a>`at`: `s64`

## <a id="demo_counter_types_0_1_0"></a>Import interface demo:counter/types@0.1.0

Qt value types, records from Q_GADGETs, and Q_ENUM/Q_FLAG enums.

----

### Types

#### <a id="mode"></a>`enum mode`

How `step` advances the value.

Q_ENUM Counter::Mode.

##### Enum Cases

- <a id="mode.linear"></a>`linear`

    Add one.

- <a id="mode.doubling"></a>`doubling`

    Double the value (zero becomes one).

## <a id="demo_counter_counter_0_1_0"></a>Export interface demo:counter/counter@0.1.0

----

### Types

#### <a id="mode"></a>`type mode`
[`mode`](#mode)

#### <a id="counter"></a>`resource counter`

A counter that steps by one or doubles.

The doc comments in this file are qdoc's. qt2wasm runs qdoc over the
project and writes them into the generated WIT as `///` docs.

----

### Functions

#### <a id="constructor_counter"></a>`[constructor]counter: func`

Creates a counter at zero in Linear mode.

##### Return values

- <a id="constructor_counter.0"></a> own<[`counter`](#counter)>

#### <a id="method_counter_id"></a>`[method]counter.id: func`

Instance id; signals from this instance carry it as `source`.

##### Params

- <a id="method_counter_id.self"></a>`self`: borrow<[`counter`](#counter)>

##### Return values

- <a id="method_counter_id.0"></a> `u32`

#### <a id="method_counter_value"></a>`[method]counter.value: func`

This property holds the current count.

Setting it records the new value in `history` and emits
`value-changed`.

Q_PROPERTY read: Counter::value

##### Params

- <a id="method_counter_value.self"></a>`self`: borrow<[`counter`](#counter)>

##### Return values

- <a id="method_counter_value.0"></a> `s32`

#### <a id="method_counter_set_value"></a>`[method]counter.set-value: func`

Sets `value`.

This property holds the current count.

Setting it records the new value in `history` and emits
`value-changed`.

Q_PROPERTY write: Counter::setValue

##### Params

- <a id="method_counter_set_value.self"></a>`self`: borrow<[`counter`](#counter)>
- <a id="method_counter_set_value.value"></a>`value`: `s32`

#### <a id="method_counter_describe"></a>`[method]counter.describe: func`

Returns `prefix` followed by the value and the mode, for example
`"n=3 (linear)"`.

##### Params

- <a id="method_counter_describe.self"></a>`self`: borrow<[`counter`](#counter)>
- <a id="method_counter_describe.prefix"></a>`prefix`: `string`

##### Return values

- <a id="method_counter_describe.0"></a> `string`

#### <a id="method_counter_history"></a>`[method]counter.history: func`

Returns the last `n` values the counter held, oldest first.

##### Params

- <a id="method_counter_history.self"></a>`self`: borrow<[`counter`](#counter)>
- <a id="method_counter_history.n"></a>`n`: `s32`

##### Return values

- <a id="method_counter_history.0"></a> list<`f64`>

#### <a id="method_counter_set_mode"></a>`[method]counter.set-mode: func`

Switches to mode `m` for the next `step`.

##### Params

- <a id="method_counter_set_mode.self"></a>`self`: borrow<[`counter`](#counter)>
- <a id="method_counter_set_mode.m"></a>`m`: [`mode`](#mode)

#### <a id="method_counter_step"></a>`[method]counter.step: func`

Advances the counter once, by the current Mode.

See also: `set-mode`, `overflowed`.

##### Params

- <a id="method_counter_step.self"></a>`self`: borrow<[`counter`](#counter)>
