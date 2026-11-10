# <a id="component"></a>demo:canvas@0.1.0 API reference

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

How the outline of a shape is drawn.

Q_GADGET Stroke.

##### Record Fields

- <a id="stroke.color"></a>`color`: [`rgba`](#rgba)

    This property holds the outline colour. The default is black.

- <a id="stroke.width"></a>`width`: `f64`

    This property holds the outline width in pixels. The default is 1.

#### <a id="shape"></a>`enum shape`

The shapes `shape` can draw.

Q_ENUM Canvas::Shape.

##### Enum Cases

- <a id="shape.box"></a>`box`

    A rectangle with rounded corners.

- <a id="shape.ellipse"></a>`ellipse`

    An ellipse filling the rectangle.

- <a id="shape.star"></a>`star`

    A five-pointed star centred in the rectangle.

#### <a id="text_options"></a>`flags text-options`

Styles for `text`; combine them as flags.

Q_FLAG Canvas::TextOptions.

##### Flags members

- <a id="text_options.bold"></a>`bold`:

    Bold weight.

- <a id="text_options.italic"></a>`italic`:

    Italic.

- <a id="text_options.underline"></a>`underline`:

    Underlined.

## <a id="demo_canvas_canvas_events_0_1_0"></a>Import interface demo:canvas/canvas-events@0.1.0

Signals of Canvas, delivered to the host as calls.

----

### Types

#### <a id="rect"></a>`type rect`
[`rect`](#rect)

----

### Functions

#### <a id="painted"></a>`painted: func`

Emitted after each drawing call: `dirty` is the area that changed.

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

An RGBA image you draw shapes and text on with QPainter.

The example shows QtGui inside a WASI component: QPainter and QFont draw
into a QImage, and the host gets the pixels back as a record from
`image`. Every drawing call emits `painted` with the area it changed.

----

### Functions

#### <a id="constructor_canvas"></a>`[constructor]canvas: func`

Creates a white canvas of `width` by `height` pixels (at least 1 by
1).

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

This property holds the canvas size in pixels, fixed when it is
created.

Q_PROPERTY read: Canvas::size

##### Params

- <a id="method_canvas_size.self"></a>`self`: borrow<[`canvas`](#canvas)>

##### Return values

- <a id="method_canvas_size.0"></a> [`size`](#size)

#### <a id="method_canvas_clear"></a>`[method]canvas.clear: func`

Fills the whole canvas with `background`.

##### Params

- <a id="method_canvas_clear.self"></a>`self`: borrow<[`canvas`](#canvas)>
- <a id="method_canvas_clear.background"></a>`background`: [`rgba`](#rgba)

#### <a id="method_canvas_shape"></a>`[method]canvas.shape: func`

Draws a `kind` shape inside `rect`, filled with `fill` and outlined
with `stroke`, antialiased.

##### Params

- <a id="method_canvas_shape.self"></a>`self`: borrow<[`canvas`](#canvas)>
- <a id="method_canvas_shape.kind"></a>`kind`: [`shape`](#shape)
- <a id="method_canvas_shape.rect"></a>`rect`: [`rect`](#rect)
- <a id="method_canvas_shape.fill"></a>`fill`: [`rgba`](#rgba)
- <a id="method_canvas_shape.stroke"></a>`stroke`: [`stroke`](#stroke)

#### <a id="method_canvas_text"></a>`[method]canvas.text: func`

Draws `text` with its baseline starting at `at`, `pixel-size` pixels
high, in `color`, styled by `options`. Returns the text's bounding
box.

The font is the first family in `font-families`: the fonts compiled
into the component.

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

Returns the font families the component can draw with.

##### Params

- <a id="method_canvas_font_families.self"></a>`self`: borrow<[`canvas`](#canvas)>

##### Return values

- <a id="method_canvas_font_families.0"></a> list<`string`>

#### <a id="method_canvas_pixel"></a>`[method]canvas.pixel: func`

Returns the colour of the pixel at `at`, or nothing when `at` is
outside the canvas.

##### Params

- <a id="method_canvas_pixel.self"></a>`self`: borrow<[`canvas`](#canvas)>
- <a id="method_canvas_pixel.at"></a>`at`: [`point`](#point)

##### Return values

- <a id="method_canvas_pixel.0"></a> option<[`rgba`](#rgba)>

#### <a id="method_canvas_image"></a>`[method]canvas.image: func`

Returns the pixels drawn so far, as RGBA.

##### Params

- <a id="method_canvas_image.self"></a>`self`: borrow<[`canvas`](#canvas)>

##### Return values

- <a id="method_canvas_image.0"></a> [`image`](#image)
