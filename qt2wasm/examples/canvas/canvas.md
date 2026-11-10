# <a id="component"></a>World component

 - Imports:
    - interface `demo:canvas/types@0.1.0`
    - interface `demo:canvas/canvas-events@0.1.0`
 - Exports:
    - interface `demo:canvas/canvas@0.1.0`

## <a id="demo_canvas_types_0_1_0"></a>Import interface demo:canvas/types@0.1.0

Qt value types, records from Q_GADGETs, and Q_ENUM/Q_FLAG enums.

----

### Types

#### <a id="image"></a>`record image`

QImage. Pixels are RGBA8888, rows tightly packed.

##### Record Fields

- <a id="image.width"></a>`width`: `u32`
- <a id="image.height"></a>`height`: `u32`
- <a id="image.pixels"></a>`pixels`: list<`u8`>

#### <a id="point"></a>`record point`

QPoint.

##### Record Fields

- <a id="point.x"></a>`x`: `s32`
- <a id="point.y"></a>`y`: `s32`

#### <a id="rect"></a>`record rect`

QRect.

##### Record Fields

- <a id="rect.x"></a>`x`: `s32`
- <a id="rect.y"></a>`y`: `s32`
- <a id="rect.width"></a>`width`: `s32`
- <a id="rect.height"></a>`height`: `s32`

#### <a id="rgba"></a>`record rgba`

QColor.

##### Record Fields

- <a id="rgba.r"></a>`r`: `u8`
- <a id="rgba.g"></a>`g`: `u8`
- <a id="rgba.b"></a>`b`: `u8`
- <a id="rgba.a"></a>`a`: `u8`

#### <a id="size"></a>`record size`

QSize.

##### Record Fields

- <a id="size.width"></a>`width`: `s32`
- <a id="size.height"></a>`height`: `s32`

#### <a id="stroke"></a>`record stroke`

Q_GADGET Stroke.

##### Record Fields

- <a id="stroke.color"></a>`color`: [`rgba`](#rgba)
- <a id="stroke.width"></a>`width`: `f64`

#### <a id="shape"></a>`enum shape`

Q_ENUM Canvas::Shape.

##### Enum Cases

- <a id="shape.box"></a>`box`
- <a id="shape.ellipse"></a>`ellipse`
- <a id="shape.star"></a>`star`

#### <a id="text_options"></a>`flags text-options`

Q_FLAG Canvas::TextOptions.

##### Flags members

- <a id="text_options.bold"></a>`bold`:
- <a id="text_options.italic"></a>`italic`:
- <a id="text_options.underline"></a>`underline`:

## <a id="demo_canvas_canvas_events_0_1_0"></a>Import interface demo:canvas/canvas-events@0.1.0

Signals of Canvas, delivered to the host as calls.

----

### Types

#### <a id="rect"></a>`type rect`
[`rect`](#rect)

----

### Functions

#### <a id="painted"></a>`painted: func`

##### Params

- <a id="painted.source"></a>`source`: `u32`
- <a id="painted.dirty"></a>`dirty`: [`rect`](#rect)

## <a id="demo_canvas_canvas_0_1_0"></a>Export interface demo:canvas/canvas@0.1.0

----

### Types

#### <a id="image"></a>`type image`
[`image`](#image)

#### <a id="point"></a>`type point`
[`point`](#point)

#### <a id="rect"></a>`type rect`
[`rect`](#rect)

#### <a id="rgba"></a>`type rgba`
[`rgba`](#rgba)

#### <a id="shape"></a>`type shape`
[`shape`](#shape)

#### <a id="size"></a>`type size`
[`size`](#size)

#### <a id="stroke"></a>`type stroke`
[`stroke`](#stroke)

#### <a id="text_options"></a>`type text-options`
[`text-options`](#text_options)

#### <a id="canvas"></a>`resource canvas`

----

### Functions

#### <a id="constructor_canvas"></a>`[constructor]canvas: func`

##### Params

- <a id="constructor_canvas.width"></a>`width`: `s32`
- <a id="constructor_canvas.height"></a>`height`: `s32`

##### Return values

- <a id="constructor_canvas.0"></a> own<[`canvas`](#canvas)>

#### <a id="method_canvas_id"></a>`[method]canvas.id: func`

Instance id; signals from this instance carry it as `source`.

##### Params

- <a id="method_canvas_id.self"></a>`self`: borrow<[`canvas`](#canvas)>

##### Return values

- <a id="method_canvas_id.0"></a> `u32`

#### <a id="method_canvas_size"></a>`[method]canvas.size: func`

Q_PROPERTY read: Canvas::size

##### Params

- <a id="method_canvas_size.self"></a>`self`: borrow<[`canvas`](#canvas)>

##### Return values

- <a id="method_canvas_size.0"></a> [`size`](#size)

#### <a id="method_canvas_clear"></a>`[method]canvas.clear: func`

##### Params

- <a id="method_canvas_clear.self"></a>`self`: borrow<[`canvas`](#canvas)>
- <a id="method_canvas_clear.background"></a>`background`: [`rgba`](#rgba)

#### <a id="method_canvas_shape"></a>`[method]canvas.shape: func`

##### Params

- <a id="method_canvas_shape.self"></a>`self`: borrow<[`canvas`](#canvas)>
- <a id="method_canvas_shape.kind"></a>`kind`: [`shape`](#shape)
- <a id="method_canvas_shape.rect"></a>`rect`: [`rect`](#rect)
- <a id="method_canvas_shape.fill"></a>`fill`: [`rgba`](#rgba)
- <a id="method_canvas_shape.stroke"></a>`stroke`: [`stroke`](#stroke)

#### <a id="method_canvas_text"></a>`[method]canvas.text: func`

##### Params

- <a id="method_canvas_text.self"></a>`self`: borrow<[`canvas`](#canvas)>
- <a id="method_canvas_text.at"></a>`at`: [`point`](#point)
- <a id="method_canvas_text.text"></a>`text`: `string`
- <a id="method_canvas_text.pixel_size"></a>`pixel-size`: `s32`
- <a id="method_canvas_text.color"></a>`color`: [`rgba`](#rgba)
- <a id="method_canvas_text.options"></a>`options`: [`text-options`](#text_options)

##### Return values

- <a id="method_canvas_text.0"></a> [`rect`](#rect)

#### <a id="method_canvas_font_families"></a>`[method]canvas.font-families: func`

##### Params

- <a id="method_canvas_font_families.self"></a>`self`: borrow<[`canvas`](#canvas)>

##### Return values

- <a id="method_canvas_font_families.0"></a> list<`string`>

#### <a id="method_canvas_pixel"></a>`[method]canvas.pixel: func`

##### Params

- <a id="method_canvas_pixel.self"></a>`self`: borrow<[`canvas`](#canvas)>
- <a id="method_canvas_pixel.at"></a>`at`: [`point`](#point)

##### Return values

- <a id="method_canvas_pixel.0"></a> option<[`rgba`](#rgba)>

#### <a id="method_canvas_image"></a>`[method]canvas.image: func`

##### Params

- <a id="method_canvas_image.self"></a>`self`: borrow<[`canvas`](#canvas)>

##### Return values

- <a id="method_canvas_image.0"></a> [`image`](#image)
