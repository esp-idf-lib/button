button: HW timer-based driver for GPIO buttons
==============================================

The driver debounces the contact, detects short and long presses and can repeat
a click while the button is held. Every button is an opaque handle with its own
timer, so several buttons work independently.

Events
------

.. list-table::
   :header-rows: 1

   * - Event
     - When
   * - ``BUTTON_PRESSED``
     - The button is pressed
   * - ``BUTTON_RELEASED``
     - The button is released
   * - ``BUTTON_CLICKED``
     - A short press ends, or an autorepeat step
   * - ``BUTTON_PRESSED_LONG``
     - The button is held longer than ``long_press_time_us``

Without autorepeat a short press produces ``PRESSED``, ``RELEASED``,
``CLICKED``, and a long press produces ``PRESSED``, ``PRESSED_LONG``,
``RELEASED``. With autorepeat the long press is disabled and holding the button
produces ``PRESSED``, a series of ``CLICKED``, then ``RELEASED``.

Modes
-----

``BUTTON_MODE_POLL`` starts a periodic timer and reads the pin every
``poll_interval_us`` microseconds. It needs no interrupt service and works with
any pin, but it wakes the CPU on every period even when nobody touches the
button.

``BUTTON_MODE_INTERRUPT`` waits for an edge on the pin; the interrupt then
disables itself and starts the same timer, which confirms the press and follows
it until the release. The interrupt service must be installed with
``gpio_install_isr_service()`` before the first such button is created. A button
that is already held at boot is not detected until it is released and pressed
again.

A pin can serve one button at a time, in either mode: the GPIO driver keeps a
single interrupt handler per pin, so a second button on the same pin would take
the handler over and the first one would stop seeing presses. The second
``button_create()`` fails with ``ESP_ERR_INVALID_STATE``, and the pin becomes
free again after ``button_delete()``.

Debouncing
----------

The samples taken during ``dead_time_us`` after a press are ignored, so the
contact bounce of the press does not produce extra events. The bounce of the
release is covered by the same window when it is shorter than the dead time;
otherwise increase ``dead_time_us``.

Callback
--------

The event callback runs in the ESP timer task while the button mutex is held. It
must be short: a delay in it delays every other ESP timer in the system. It also
must not call the driver back, because the mutex is not recursive: deleting the
button from the callback deadlocks.

Reference
---------

 .. doxygenfile:: button.h
