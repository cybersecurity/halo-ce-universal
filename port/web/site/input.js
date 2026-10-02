/*
INPUT.JS

The player's input, gathered on the page's main thread and handed to the
game through the shared state (port/web/src/web_shared.h):

- the keyboard and the mouse (with the pointer locked), as SDL events,
  which the Linux port's keyboard-and-mouse controls read
  (port/linux/README.md, "Controls");
- game controllers, through the Gamepad API's standard mapping;
- touch controls for phones and tablets without a controller: a stick for
  moving, dragging for aiming (as the mouse does) and the controller's
  buttons, drawn over the game.
*/

'use strict';

const HaloInput = (() => {
  const EVENT = { KEY: 1, MOUSE_MOTION: 2, MOUSE_BUTTON: 3, MOUSE_WHEEL: 4, FOCUS: 5, GAMEPAD_ADDED: 6 };
  // SDL_GamepadButton
  const BUTTON = {
    SOUTH: 0, EAST: 1, WEST: 2, NORTH: 3, BACK: 4, GUIDE: 5, START: 6, LEFT_STICK: 7, RIGHT_STICK: 8,
    LEFT_SHOULDER: 9, RIGHT_SHOULDER: 10, DPAD_UP: 11, DPAD_DOWN: 12, DPAD_LEFT: 13, DPAD_RIGHT: 14,
  };
  const GAMEPAD_TYPE_XBOXONE = 3;
  const GAMEPAD_TYPE_PS4 = 5;
  const GAMEPAD_TYPE_PS5 = 6;
  const TOUCH_SLOT = 4;
  const TOUCH_ID = 1000;

  // KeyboardEvent.code -> SDL_Scancode (USB HID usage)
  const SCANCODES = {};
  'abcdefghijklmnopqrstuvwxyz'.split('').forEach((c, i) => { SCANCODES['Key' + c.toUpperCase()] = 4 + i; });
  '1234567890'.split('').forEach((c, i) => { SCANCODES['Digit' + c] = 30 + i; });
  Object.assign(SCANCODES, {
    Enter: 40, Escape: 41, Backspace: 42, Tab: 43, Space: 44, Minus: 45, Equal: 46, BracketLeft: 47,
    BracketRight: 48, Backslash: 49, Semicolon: 51, Quote: 52, Backquote: 53, Comma: 54, Period: 55, Slash: 56,
    CapsLock: 57, PrintScreen: 70, ScrollLock: 71, Pause: 72, Insert: 73, Home: 74, PageUp: 75, Delete: 76,
    End: 77, PageDown: 78, ArrowRight: 79, ArrowLeft: 80, ArrowDown: 81, ArrowUp: 82, NumLock: 83,
    NumpadDivide: 84, NumpadMultiply: 85, NumpadSubtract: 86, NumpadAdd: 87, NumpadEnter: 88,
    Numpad1: 89, Numpad2: 90, Numpad3: 91, Numpad4: 92, Numpad5: 93, Numpad6: 94, Numpad7: 95, Numpad8: 96,
    Numpad9: 97, Numpad0: 98, NumpadDecimal: 99, ControlLeft: 224, ShiftLeft: 225, AltLeft: 226, MetaLeft: 227,
    ControlRight: 228, ShiftRight: 229, AltRight: 230, MetaRight: 231,
  });
  for (let i = 1; i <= 12; i++) SCANCODES['F' + i] = 57 + i;

  let shared = null; // { i32, f32, offsets, base }
  let touchEnabled = false;
  let touchUsed = false;
  let lookSensitivity = 1.4;
  let uiActive = false;
  const gamepadIds = new Map(); // Gamepad.index -> slot
  let nextGamepadId = 1;
  const touchPad = { buttons: 0, axes: [0, 0, 0, 0, 0, 0] };
  const rumbleSeen = [0, 0, 0, 0, 0];

  function word(offset) { return (shared.base + offset) >> 2; }

  function pushEvent(type, a = 0, b = 0, c = 0, d = 0, x = 0, y = 0, extra = 0) {
    if (!shared) return;
    const { i32, f32, offsets } = shared;
    const writeIndex = word(offsets.eventWrite);
    const readIndex = word(offsets.eventRead);
    const write = Atomics.load(i32, writeIndex);
    const read = Atomics.load(i32, readIndex);
    if (write - read >= offsets.eventCapacity) return;
    const slot = word(offsets.events + (write % offsets.eventCapacity) * offsets.eventSize);
    i32[slot] = type;
    i32[slot + 1] = a;
    i32[slot + 2] = b;
    i32[slot + 3] = c;
    i32[slot + 4] = d;
    f32[slot + 5] = x;
    f32[slot + 6] = y;
    i32[slot + 7] = extra;
    Atomics.store(i32, writeIndex, write + 1);
  }

  // ---------- keyboard

  function keycode(event, scancode) {
    if (event.key && event.key.length === 1) return event.key.toLowerCase().codePointAt(0);
    const named = { Enter: 13, Escape: 27, Backspace: 8, Tab: 9, ' ': 32, Delete: 127 };
    if (named[event.key] !== undefined) return named[event.key];
    return (scancode | (1 << 30)) >>> 0;
  }

  function modifiers(event) {
    let mod = 0;
    if (event.shiftKey) mod |= 0x0001;
    if (event.ctrlKey) mod |= 0x0040;
    if (event.altKey) mod |= 0x0100;
    if (event.metaKey) mod |= 0x0400;
    if (event.getModifierState && event.getModifierState('CapsLock')) mod |= 0x2000;
    return mod;
  }

  function onKey(event, down) {
    if (!shared || uiActive || event.target?.closest?.('input, textarea, select, [contenteditable="true"]')) return;
    const scancode = SCANCODES[event.code];
    if (scancode === undefined) return;
    // keep the browser's shortcuts (reload, developer tools) with Ctrl/Cmd
    if (!(event.metaKey || (event.ctrlKey && event.code !== 'ControlLeft' && event.code !== 'ControlRight'))) {
      event.preventDefault();
    }
    pushEvent(EVENT.KEY, scancode, down ? 1 : 0, event.repeat ? 1 : 0, keycode(event, scancode) | 0, 0, 0,
      modifiers(event));
  }

  // ---------- mouse (with the pointer locked on the game)

  function mouseButton(button) {
    return [1, 2, 3, 4, 5][button] || 0; // left, middle, right, back, forward
  }

  function attachMouse(canvas) {
    canvas.addEventListener('click', () => {
      if (!document.pointerLockElement && canvas.requestPointerLock && matchMedia('(pointer: fine)').matches) {
        try { canvas.requestPointerLock(); } catch { /* not allowed here */ }
      }
    });
    document.addEventListener('mousemove', (event) => {
      if (document.pointerLockElement !== canvas) return;
      pushEvent(EVENT.MOUSE_MOTION, 0, 0, 0, 0, event.movementX || 0, event.movementY || 0);
    });
    document.addEventListener('mousedown', (event) => {
      if (document.pointerLockElement !== canvas) return;
      pushEvent(EVENT.MOUSE_BUTTON, mouseButton(event.button), 1);
    });
    document.addEventListener('mouseup', (event) => {
      if (document.pointerLockElement !== canvas) return;
      pushEvent(EVENT.MOUSE_BUTTON, mouseButton(event.button), 0);
    });
    document.addEventListener('wheel', (event) => {
      if (document.pointerLockElement !== canvas) return;
      pushEvent(EVENT.MOUSE_WHEEL, 0, 0, 0, 0, 0, event.deltaY < 0 ? 1 : event.deltaY > 0 ? -1 : 0);
    }, { passive: true });
  }

  // ---------- controllers

  function gamepadType(gamepad) {
    const id = (gamepad.id || '').toLowerCase();
    if (id.includes('dualsense') || id.includes('0ce6')) return GAMEPAD_TYPE_PS5;
    if (id.includes('dualshock') || id.includes('054c')) return GAMEPAD_TYPE_PS4;
    return GAMEPAD_TYPE_XBOXONE;
  }

  function axis(value) {
    return Math.max(-32768, Math.min(32767, Math.round(value * 32767)));
  }

  function writeGamepad(slot, connected, id, type, buttons, axes) {
    const { i32, offsets } = shared;
    const base = word(offsets.gamepads + slot * offsets.gamepadSize);
    const wasConnected = i32[base] !== 0;
    i32[base + 1] = id;
    i32[base + 2] = type;
    i32[base + 3] = buttons;
    for (let i = 0; i < 6; i++) i32[base + 4 + i] = axes[i];
    Atomics.store(i32, base, connected ? 1 : 0);
    if (connected && !wasConnected) pushEvent(EVENT.GAMEPAD_ADDED, id);
  }

  function rumble(slot, gamepad) {
    const { i32, offsets } = shared;
    const base = word(offsets.gamepads + slot * offsets.gamepadSize);
    const serial = i32[base + 12];
    if (serial === rumbleSeen[slot]) return;
    rumbleSeen[slot] = serial;
    const actuator = gamepad && gamepad.vibrationActuator;
    if (!actuator || !actuator.playEffect) return;
    const strong = i32[base + 10] / 65535;
    const weak = i32[base + 11] / 65535;
    const duration = Math.min(i32[base + 13] || 1000, 5000);
    if (strong <= 0 && weak <= 0) {
      actuator.reset?.();
      return;
    }
    actuator.playEffect('dual-rumble', { duration, strongMagnitude: strong, weakMagnitude: weak }).catch(() => {});
  }

  let physicalCount = 0;

  function pollGamepads() {
    const pads = navigator.getGamepads ? navigator.getGamepads() : [];
    const used = new Set();
    physicalCount = 0;
    for (const gamepad of pads) {
      if (!gamepad || !gamepad.connected) continue;
      let slot = gamepadIds.get(gamepad.index);
      if (slot === undefined) {
        for (let s = 0; s < TOUCH_SLOT; s++) {
          if (![...gamepadIds.values()].includes(s)) { slot = s; break; }
        }
        if (slot === undefined) continue;
        gamepadIds.set(gamepad.index, slot);
        slot = gamepadIds.get(gamepad.index);
        shared.ids = shared.ids || [];
        shared.ids[slot] = nextGamepadId++;
      }
      used.add(slot);
      physicalCount++;
      const b = gamepad.buttons;
      const pressed = (i) => (b[i] ? (b[i].pressed || b[i].value > 0.5) : false);
      let buttons = 0;
      const map = [
        [0, BUTTON.SOUTH], [1, BUTTON.EAST], [2, BUTTON.WEST], [3, BUTTON.NORTH], [4, BUTTON.LEFT_SHOULDER],
        [5, BUTTON.RIGHT_SHOULDER], [8, BUTTON.BACK], [9, BUTTON.START], [10, BUTTON.LEFT_STICK],
        [11, BUTTON.RIGHT_STICK], [12, BUTTON.DPAD_UP], [13, BUTTON.DPAD_DOWN], [14, BUTTON.DPAD_LEFT],
        [15, BUTTON.DPAD_RIGHT], [16, BUTTON.GUIDE],
      ];
      for (const [index, bit] of map) if (pressed(index)) buttons |= 1 << bit;
      const a = gamepad.axes;
      const trigger = (i) => (b[i] ? Math.round(Math.max(0, Math.min(1, b[i].value)) * 32767) : 0);
      writeGamepad(slot, true, shared.ids[slot], gamepadType(gamepad), buttons,
        [axis(a[0] || 0), axis(a[1] || 0), axis(a[2] || 0), axis(a[3] || 0), trigger(6), trigger(7)]);
      rumble(slot, gamepad);
    }
    for (const [index, slot] of [...gamepadIds.entries()]) {
      if (!used.has(slot)) {
        gamepadIds.delete(index);
        writeGamepad(slot, false, 0, 0, 0, [0, 0, 0, 0, 0, 0]);
      }
    }
    // the touch controls are a controller of their own while no other is
    const touchActive = touchEnabled && touchUsed && physicalCount === 0;
    writeGamepad(TOUCH_SLOT, touchActive, TOUCH_ID, GAMEPAD_TYPE_XBOXONE, touchPad.buttons, touchPad.axes);
    document.body.classList.toggle('has-controller', physicalCount > 0);
  }

  // ---------- touch controls

  function buildCompactTouchControls(root) {
    root.classList.add('fps-controls');
    // Compact actions; movement and aiming are independent multitouch regions.
    const layout = [
      ['fire', '◎', { axis: 5 }, 'fps-fire', 'Fire'],
      ['a', '↑', { bit: BUTTON.SOUTH }, 'fps-jump', 'Jump'],
      ['b', '╱', { bit: BUTTON.EAST }, 'fps-melee', 'Melee'],
      ['x', '↻', { bit: BUTTON.WEST }, 'fps-reload', 'Reload / Use'],
      ['y', '', { bit: BUTTON.NORTH }, 'fps-weapon-hud', ''],
      ['lt', '●', { axis: 4 }, 'fps-grenade', 'Throw'],
      ['black', '⇆', { bit: BUTTON.RIGHT_SHOULDER }, 'fps-grenade-type', 'Grenade'],
      ['ls', '⌄', { bit: BUTTON.LEFT_STICK }, 'fps-crouch', 'Crouch'],
      ['rs', '⊕', { bit: BUTTON.RIGHT_STICK }, 'fps-zoom', 'Zoom'],
    ];
    const held = new Map(); // touch identifier -> control
    const buttonState = new Map(); // control id -> count of touches
    const stick = { id: null, x: 0, y: 0, element: null, knob: null };
    const look = { id: null, x: 0, y: 0 };

    const stickBase = document.createElement('div');
    stickBase.className = 'touch-stick';
    const knob = document.createElement('div');
    knob.className = 'touch-knob';
    stickBase.appendChild(knob);
    root.appendChild(stickBase);
    stick.element = stickBase;
    stick.knob = knob;

    const controls = {};
    for (const [id, label, action, cls, caption] of layout) {
      const element = document.createElement('div');
      element.className = 'touch-button ' + cls;
      element.dataset.control = id;
      element.setAttribute('aria-label', id === 'y' ? 'Swap weapon' : caption || id);
      if (label) {
        const glyph = document.createElement('span');
        glyph.className = 'glyph';
        glyph.textContent = label;
        element.appendChild(glyph);
      }
      if (caption) {
        const hint = document.createElement('span');
        hint.className = 'caption';
        hint.textContent = caption;
        element.appendChild(hint);
      }
      root.appendChild(element);
      controls[id] = { element, action };
    }

    function refreshButtons() {
      let buttons = 0;
      touchPad.axes[4] = 0;
      touchPad.axes[5] = 0;
      for (const [id, count] of buttonState) {
        if (count <= 0) continue;
        const { action } = controls[id];
        if (action.bit !== undefined) buttons |= 1 << action.bit;
        if (action.axis !== undefined) touchPad.axes[action.axis] = 32767;
      }
      touchPad.buttons = buttons;
      for (const id in controls) controls[id].element.classList.toggle('pressed', (buttonState.get(id) || 0) > 0);
    }

    const STICK_RADIUS = 40;

    function start(event) {
      pushEvent(EVENT.FOCUS, 1);
      touchUsed = true;
      for (const touch of event.changedTouches) {
        const target = document.elementFromPoint(touch.clientX, touch.clientY);
        const control = target && target.closest && target.closest('.touch-button');
        if (control) {
          const id = control.dataset.control;
          held.set(touch.identifier, { kind: 'button', id });
          buttonState.set(id, (buttonState.get(id) || 0) + 1);
          // aiming while firing: a touch on the fire button also aims
          if (id === 'fire' && look.id === null) {
            look.id = touch.identifier;
            look.x = touch.clientX;
            look.y = touch.clientY;
          }
          continue;
        }
        if (touch.clientX < window.innerWidth * 0.38 && stick.id === null) {
          stick.id = touch.identifier;
          stick.x = touch.clientX;
          stick.y = touch.clientY;
          stick.element.style.left = (touch.clientX - 48) + 'px';
          stick.element.style.top = (touch.clientY - 48) + 'px';
          stick.element.style.bottom = 'auto';
          stick.element.classList.add('active');
          held.set(touch.identifier, { kind: 'stick' });
        } else if (look.id === null) {
          look.id = touch.identifier;
          look.x = touch.clientX;
          look.y = touch.clientY;
          held.set(touch.identifier, { kind: 'look' });
        }
      }
      refreshButtons();
      event.preventDefault();
    }

    function move(event) {
      for (const touch of event.changedTouches) {
        if (touch.identifier === stick.id) {
          let dx = touch.clientX - stick.x;
          let dy = touch.clientY - stick.y;
          const length = Math.hypot(dx, dy);
          if (length > STICK_RADIUS) {
            dx *= STICK_RADIUS / length;
            dy *= STICK_RADIUS / length;
          }
          stick.knob.style.transform = `translate(${dx}px, ${dy}px)`;
          touchPad.axes[0] = axis(dx / STICK_RADIUS);
          touchPad.axes[1] = axis(dy / STICK_RADIUS);
        }
        if (touch.identifier === look.id) {
          const dx = touch.clientX - look.x;
          const dy = touch.clientY - look.y;
          look.x = touch.clientX;
          look.y = touch.clientY;
          pushEvent(EVENT.MOUSE_MOTION, 0, 0, 0, 0, dx * lookSensitivity * 2, dy * lookSensitivity * 2);
        }
      }
      event.preventDefault();
    }

    function end(event) {
      for (const touch of event.changedTouches) {
        const entry = held.get(touch.identifier);
        held.delete(touch.identifier);
        if (entry && entry.kind === 'button') {
          buttonState.set(entry.id, Math.max(0, (buttonState.get(entry.id) || 0) - 1));
        }
        if (touch.identifier === stick.id) {
          stick.id = null;
          stick.knob.style.transform = '';
          stick.element.classList.remove('active');
          // back to its resting place
          stick.element.style.left = '';
          stick.element.style.top = '';
          stick.element.style.bottom = '';
          touchPad.axes[0] = 0;
          touchPad.axes[1] = 0;
        }
        if (touch.identifier === look.id) look.id = null;
      }
      refreshButtons();
      event.preventDefault();
    }

    function releaseTouches() {
      held.clear(); buttonState.clear();
      stick.id = null; look.id = null;
      touchPad.axes.fill(0); touchPad.buttons = 0;
      stick.knob.style.transform = '';
      stick.element.classList.remove('active');
      stick.element.style.left = ''; stick.element.style.top = ''; stick.element.style.bottom = '';
      refreshButtons();
    }
    window.addEventListener('blur', releaseTouches);
    window.addEventListener('resize', releaseTouches);
    window.addEventListener('gamepadconnected', releaseTouches);
    document.addEventListener('visibilitychange', () => { if (document.hidden) releaseTouches(); });
    root.addEventListener('contextmenu', event => event.preventDefault());
    root.addEventListener('touchstart', start, { passive: false });
    root.addEventListener('touchmove', move, { passive: false });
    root.addEventListener('touchend', end, { passive: false });
    root.addEventListener('touchcancel', end, { passive: false });
  }


  function buildTouchControls(root) {
    // the original Xbox controller's layout (the Controller S): gem-coloured
    // A, B, X and Y in a diamond with the white and black buttons below
    // them, the L and R triggers above each side, back and start between
    // the thumbs, and the d-pad under the left thumb
    const layout = [
      // [id, label, button bit or trigger axis, class, caption]
      ['a', 'A', { bit: BUTTON.SOUTH }, 'face face-a', 'Jump'],
      ['b', 'B', { bit: BUTTON.EAST }, 'face face-b', 'Melee'],
      ['x', 'X', { bit: BUTTON.WEST }, 'face face-x', 'Reload'],
      ['y', 'Y', { bit: BUTTON.NORTH }, 'face face-y', 'Swap'],
      ['white', '', { bit: BUTTON.LEFT_SHOULDER }, 'duo duo-white', 'Light'],
      ['black', '', { bit: BUTTON.RIGHT_SHOULDER }, 'duo duo-black', 'Grenade'],
      ['fire', 'R', { axis: 5 }, 'trigger right-side', 'Fire'],
      ['lt', 'L', { axis: 4 }, 'trigger left-side', 'Throw'],
      ['ls', '', { bit: BUTTON.LEFT_STICK }, 'stick-click left-click', 'Crouch'],
      ['rs', '', { bit: BUTTON.RIGHT_STICK }, 'stick-click right-click', 'Zoom'],
      ['back', 'BACK', { bit: BUTTON.BACK }, 'system back', ''],
      ['start', 'START', { bit: BUTTON.START }, 'system start', ''],
      ['up', '', { bit: BUTTON.DPAD_UP }, 'dpad up', ''],
      ['down', '', { bit: BUTTON.DPAD_DOWN }, 'dpad down', ''],
      ['left', '', { bit: BUTTON.DPAD_LEFT }, 'dpad left', ''],
      ['right', '', { bit: BUTTON.DPAD_RIGHT }, 'dpad right', ''],
    ];
    const held = new Map(); // touch identifier -> control
    const buttonState = new Map(); // control id -> count of touches
    const stick = { id: null, x: 0, y: 0, element: null, knob: null };
    const look = { id: null, x: 0, y: 0 };

    const stickBase = document.createElement('div');
    stickBase.className = 'touch-stick';
    const knob = document.createElement('div');
    knob.className = 'touch-knob';
    stickBase.appendChild(knob);
    root.appendChild(stickBase);
    stick.element = stickBase;
    stick.knob = knob;

    const controls = {};
    const dpad = document.createElement('div');
    dpad.className = 'touch-dpad';
    root.appendChild(dpad);
    for (const [id, label, action, cls, caption] of layout) {
      const element = document.createElement('div');
      element.className = 'touch-button ' + cls;
      element.dataset.control = id;
      element.setAttribute('aria-label', caption || id);
      if (label) {
        const glyph = document.createElement('span');
        glyph.className = 'glyph';
        glyph.textContent = label;
        element.appendChild(glyph);
      }
      if (caption) {
        const hint = document.createElement('span');
        hint.className = 'caption';
        hint.textContent = caption;
        element.appendChild(hint);
      }
      (cls.startsWith('dpad') ? dpad : root).appendChild(element);
      controls[id] = { element, action };
    }

    function refreshButtons() {
      let buttons = 0;
      touchPad.axes[4] = 0;
      touchPad.axes[5] = 0;
      for (const [id, count] of buttonState) {
        if (count <= 0) continue;
        const { action } = controls[id];
        if (action.bit !== undefined) buttons |= 1 << action.bit;
        if (action.axis !== undefined) touchPad.axes[action.axis] = 32767;
      }
      touchPad.buttons = buttons;
      for (const id in controls) controls[id].element.classList.toggle('pressed', (buttonState.get(id) || 0) > 0);
    }

    const STICK_RADIUS = 56;

    function start(event) {
      touchUsed = true;
      for (const touch of event.changedTouches) {
        const target = document.elementFromPoint(touch.clientX, touch.clientY);
        const control = target && target.closest && target.closest('.touch-button');
        if (control) {
          const id = control.dataset.control;
          held.set(touch.identifier, { kind: 'button', id });
          buttonState.set(id, (buttonState.get(id) || 0) + 1);
          // aiming while firing: a touch on the fire button also aims
          if (id === 'fire' && look.id === null) {
            look.id = touch.identifier;
            look.x = touch.clientX;
            look.y = touch.clientY;
          }
          continue;
        }
        if (touch.clientX < window.innerWidth * 0.4 && stick.id === null) {
          stick.id = touch.identifier;
          stick.x = touch.clientX;
          stick.y = touch.clientY;
          stick.element.style.left = (touch.clientX - 64) + 'px';
          stick.element.style.top = (touch.clientY - 64) + 'px';
          stick.element.style.bottom = 'auto';
          stick.element.classList.add('active');
          held.set(touch.identifier, { kind: 'stick' });
        } else if (look.id === null) {
          look.id = touch.identifier;
          look.x = touch.clientX;
          look.y = touch.clientY;
          held.set(touch.identifier, { kind: 'look' });
        }
      }
      refreshButtons();
      event.preventDefault();
    }

    function move(event) {
      for (const touch of event.changedTouches) {
        if (touch.identifier === stick.id) {
          let dx = touch.clientX - stick.x;
          let dy = touch.clientY - stick.y;
          const length = Math.hypot(dx, dy);
          if (length > STICK_RADIUS) {
            dx *= STICK_RADIUS / length;
            dy *= STICK_RADIUS / length;
          }
          stick.knob.style.transform = `translate(${dx}px, ${dy}px)`;
          touchPad.axes[0] = axis(dx / STICK_RADIUS);
          touchPad.axes[1] = axis(dy / STICK_RADIUS);
        }
        if (touch.identifier === look.id) {
          const dx = touch.clientX - look.x;
          const dy = touch.clientY - look.y;
          look.x = touch.clientX;
          look.y = touch.clientY;
          pushEvent(EVENT.MOUSE_MOTION, 0, 0, 0, 0, dx * lookSensitivity * 2, dy * lookSensitivity * 2);
        }
      }
      event.preventDefault();
    }

    function end(event) {
      for (const touch of event.changedTouches) {
        const entry = held.get(touch.identifier);
        held.delete(touch.identifier);
        if (entry && entry.kind === 'button') {
          buttonState.set(entry.id, Math.max(0, (buttonState.get(entry.id) || 0) - 1));
        }
        if (touch.identifier === stick.id) {
          stick.id = null;
          stick.knob.style.transform = '';
          stick.element.classList.remove('active');
          // back to its resting place
          stick.element.style.left = '';
          stick.element.style.top = '';
          stick.element.style.bottom = '';
          touchPad.axes[0] = 0;
          touchPad.axes[1] = 0;
        }
        if (touch.identifier === look.id) look.id = null;
      }
      refreshButtons();
      event.preventDefault();
    }

    root.addEventListener('touchstart', start, { passive: false });
    root.addEventListener('touchmove', move, { passive: false });
    root.addEventListener('touchend', end, { passive: false });
    root.addEventListener('touchcancel', end, { passive: false });
  }

  // ---------- setup

  function attach({ memory, base, offsets, canvas, touchRoot, touch, touchFps = false }) {
    const buffer = memory.buffer;
    shared = { i32: new Int32Array(buffer), f32: new Float32Array(buffer), base, offsets, ids: [] };
    window.addEventListener('keydown', (event) => onKey(event, true));
    window.addEventListener('keyup', (event) => onKey(event, false));
    window.addEventListener('blur', () => pushEvent(EVENT.FOCUS, 0));
    window.addEventListener('focus', () => pushEvent(EVENT.FOCUS, 1));
    attachMouse(canvas);
    touchEnabled = touch;
    if (touch) {
      if (touchFps) buildCompactTouchControls(touchRoot);
      else buildTouchControls(touchRoot);
      touchUsed = true;
    }
  }

  function setLookSensitivity(value) {
    lookSensitivity = value;
  }

  function setUIActive(active) {
    uiActive = active;
    pushEvent(EVENT.FOCUS, active ? 0 : 1);
  }

  // the controller's B for a moment (the system's back gesture): the
  // keyboard's Backspace, which the controller emulation reads as B
  function pressBack() {
    pushEvent(EVENT.KEY, 42, 1, 0, 8);
    setTimeout(() => pushEvent(EVENT.KEY, 42, 0, 0, 8), 120);
  }

  return { attach, pollGamepads, setLookSensitivity, pressBack, setUIActive };
})();
