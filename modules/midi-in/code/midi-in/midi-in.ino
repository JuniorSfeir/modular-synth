// MIDI library from https://github.com/FortySevenEffects/arduino_midi_library
#include <MIDI.h>
#include <midi_Defs.h>
#include <midi_Message.h>
#include <midi_Namespace.h>
#include <midi_Settings.h>
#include <EEPROM.h>
#include <math.h>

// Constant values
#define MIDI_CHANNELS 16
// Wait this long before accepting another button press
#define BUTTON_DEBOUNCE_MS 150
// Allows shift registers to reset on power-up
#define POWERUP_MS 200
// Where in the EEPROM we store the selected channel
#define MIDI_CHANNEL_EEPROM_ADDRESS 0
// Analog outputs are made symmetrical around 0, this value
// sits in the center of the DAC output range of 0-1023, setting
// The output to 0V.
#define ANALOG_CENTER_VALUE 512
// Constraining MIDI notes because of limit output voltage range
// C2
#define MIDI_MIN_NOTE 24
// C9
#define MIDI_MAX_NOTE 108
// Not including value 0, as we're zero-indexed
#define MIDI_NOTE_RANGE 84
#define MIDI_NOTES_PER_OCTAVE 12
// How many historical notes we keep track off,
// Allowing us to return to them when newer notes get released
#define MIDI_NOTE_STACK_SIZE 24

// MIDI control identifiers
#define MIDIC_MODULATION 0b00000001
#define MIDIC_VOLUME     0b00000111
#define MIDIC_BALANCE    0b00001000
#define MIDIC_PAN        0b00001010

// Addresses taken from LTC1660 datasheet
// The weird order of DACs assigned here is due ease of PCB routing.
#define DAC_ADDR_FREQ 0b0100 // DAC D
#define DAC_ADDR_MOD1 0b0011 // DAC C, Modulation
#define DAC_ADDR_MOD2 0b0010 // DAC B, Volume
#define DAC_ADDR_MOD3 0b0001 // DAC A, Balance
#define DAC_ADDR_MOD4 0b1000 // DAC H, Pitch Bend
// 5 - 7 currently unused, though to be available as output of the 
// MIDI-in board.
#define DAC_ADDR_MOD5 0b0111 // DAC G, Unassigned
#define DAC_ADDR_MOD6 0b0110 // DAC F, Unassigned
#define DAC_ADDR_MOD7 0b0101 // DAC E, Unassigned
// Special address that loads the value into all DAC output
#define DAC_ADDR_ALL  0b1111

// Output pins
#define PIN_GATE 7
#define PIN_TRIGGER 8

// 7 Segment-related
#define PIN_DISP_LATCH 19
#define PIN_DISP_CLK 4
#define PIN_DISP_DATA 18

// DAC-related
#define PIN_DAC_LATCH 9
#define PIN_DAC_CLK 6
#define PIN_DAC_DATA 5

// Input pins
// Channel select button
#define PIN_CHANNEL_SELECT_BUTTON 2

//Initialize the MIDI library
MIDI_CREATE_DEFAULT_INSTANCE();

// Array of precalculated values per note (see setup())
int noteOutputValues[MIDI_NOTE_RANGE + 1];

// Contains active notes in order of when they
// were pressed. While we only have one frequency output,
// this allows returning to the last pressed note when the
// most recent one is released, provided the previous one is still held down.
byte activeNoteStack[MIDI_NOTE_STACK_SIZE];
byte activeNoteStackSize = 0;

// Hard-coded array of 7segment sequences.
// Values set up in setup()
byte DIGITS[10];

// The MIDI channel we're interested in
volatile byte midiChannel = 0;

// Used for debouncing channel select button
volatile long lastButtonPress = 0;

// Number of keys being pressed on keyboard
volatile unsigned short notesOn = 0;

// Note currently being played on frequency output
volatile byte activeNote;

volatile byte currentVelocity;

void setup() {
  // Serial for display
  pinMode(PIN_DISP_LATCH, OUTPUT);
  pinMode(PIN_DISP_CLK, OUTPUT);
  pinMode(PIN_DISP_DATA, OUTPUT);

  // Serial for DAC
  pinMode(PIN_DAC_LATCH, OUTPUT);
  pinMode(PIN_DAC_CLK, OUTPUT);
  pinMode(PIN_DAC_DATA, OUTPUT);

  pinMode(PIN_GATE, OUTPUT);
  pinMode(PIN_TRIGGER, OUTPUT);
  pinMode(PIN_CHANNEL_SELECT_BUTTON, INPUT);

  // Set up 7 segment digit bytes
  // Segments:  ABCDEFGdp
  DIGITS[0] = 0b11111100;
  DIGITS[1] = 0b01100000;
  DIGITS[2] = 0b11011010;
  DIGITS[3] = 0b11110010;
  DIGITS[4] = 0b01100110;
  DIGITS[5] = 0b10110110;
  DIGITS[6] = 0b10111110;
  DIGITS[7] = 0b11100000;
  DIGITS[8] = 0b11111110;
  DIGITS[9] = 0b11110110;

  // Listen to button presses for the channel select button
  attachInterrupt(
    digitalPinToInterrupt(PIN_CHANNEL_SELECT_BUTTON),
    channelSelectButtonOnPressed,
    FALLING);

  // Ensure the display shift registers don't think we're writing
  digitalWrite(PIN_DISP_LATCH, LOW);

  // Ensure the DAC shift register doesn't think we're writing
  digitalWrite(PIN_DAC_LATCH, HIGH);

  // Begin listening for midi messages
  // We listen to all channels so we can change the channel
  // at runtime and filter ourselves. This means we don't
  // have to hardcode the channel.
  MIDI.begin(MIDI_CHANNEL_OMNI);

  // Allow some time for the power-up reset circuit to do its thing,
  // resetting the shift registers before we pump data into them
  delay(POWERUP_MS);

  // Calculate output values for each supported note
  float noteStep = (float)1024 / (float)MIDI_NOTE_RANGE;

  // Precalculate all the note values, so we don't have to
  // do it during normal runtime. Takes up some memory, but
  // I think it's worth it.
  for (byte i = 0; i <= MIDI_NOTE_RANGE; i++) {
    noteOutputValues[i] = (int)round(noteStep * (float)i);
  }

  // Retrieve last MIDI channel used from EEPROM
  setMIDIChannel(EEPROM.read(MIDI_CHANNEL_EEPROM_ADDRESS));
}

void loop() {
  // If we've received a new MIDI message
  if (MIDI.read())
  {
    // Ignore messages not destined for our channel
    if (MIDI.getChannel() !=  midiChannel) return;
    
    // Otherwise, process it.
    processMIDIMessage();
  }
}

void processMIDIMessage() {
  switch (MIDI.getType()) {
    case midi::NoteOn:
      // If we already had one or more notes pressed down,
      // Save the currently active note to the stack
      if (notesOn > 0) {        
        pushToNoteStack(activeNote);
      }
      
      // Set the new active note from the MIDI message
      activeNote = MIDI.getData1();
      
      // TODO: ideally this velocity uses the same system as the note,
      // that is, with a note stack. Would be better to combine the two
      // with a bit of a refactor.
      currentVelocity = MIDI.getData2();
      notesOn++;
      
      sendTriggerPulse();
      updateGate();
      updateFreqOutput();
      updateVelocityOutput();
      break;

    case midi::NoteOff:
      notesOn--;
      
      // If we still have at least one note on,
      // figure out what to do next.
      if (notesOn > 0) {
        byte releasedNote = MIDI.getData1();
        // If the released note is the one we last pressed,
        // return to the previously pressed note (that is still held).
        // Otherwise, remove the newly released notes from the stack
        // of held notes, so we don't return to it later.
        if (releasedNote == activeNote) {
          activeNote = popFromNoteStack();          
          updateFreqOutput();
        } else {
          removeFromNoteStack(releasedNote);
        }        
      }

      updateGate();
      break;

    case midi::ControlChange:
      outputControlChange(MIDI.getData1(), MIDI.getData2());
      break;

    case midi::PitchBend:
      outputPitchBend(MIDI.getData1(), MIDI.getData2());      
      break;
  }
};

// Considering the limited range of our output control voltage
// we can't cover the full range of notes MIDI covers.
// We cover C2-C9.
// Notes outside the range are transposed an octave at a time
// until they fall within our range.
byte remapNote(byte inputNote) {
  byte outputNote = inputNote;

  // Raise octave while note is below range
  while (outputNote < MIDI_MIN_NOTE) outputNote += MIDI_NOTES_PER_OCTAVE;
  // Lower octave while note is above range
  while (outputNote > MIDI_MAX_NOTE) outputNote -= MIDI_NOTES_PER_OCTAVE;

  // Remove offset, so we're once again 0-based
  return outputNote - MIDI_MIN_NOTE;
}

// --- Active (held) note management

// Add a note to the note stack
void pushToNoteStack(byte note) {
  // If our stack is already full, remove the oldest
  // note from it by shifting. Otherwise, increment.
  if (activeNoteStackSize == MIDI_NOTE_STACK_SIZE) {
    shiftNoteStack(0);
  }

  activeNoteStackSize++;
  
  activeNoteStack[activeNoteStackSize - 1] = note;
}

// Remove the latest note from the note stack and return it
byte popFromNoteStack() {
  byte note = activeNoteStack[activeNoteStackSize - 1];
  activeNoteStack[activeNoteStackSize - 1] = 0;
  activeNoteStackSize--;
  
  return note;
}

// Removes a given note from the active note stack,
// shifting any later notes down in the stack
void removeFromNoteStack(byte note) {
  byte stackIndex;
  
  // Locate index of active note, within the size of activeNoteStackSize
  for (stackIndex = 0; stackIndex < activeNoteStackSize; stackIndex++) {
    // Break from the loop when found, which keeps stackIndex at the
    // matching index
    if (activeNoteStack[stackIndex] == note) break;
  }
  
  // Remove the note at the found index from the stack
  shiftNoteStack(stackIndex);
}

// Remove an entry from the note stack, moving down
// all values stored at a higher index to remove the gap
void shiftNoteStack(byte startIndex) {
  for (byte i = startIndex; i < MIDI_NOTE_STACK_SIZE - 1; i++) {
    activeNoteStack[i] = activeNoteStack[i + 1];
  }
  
  // Granted, 0 is a valid value, but it shouldn't
  // be retrieved anyway, considering notesOn keeps
  // track of the topmost index.
  activeNoteStack[MIDI_NOTE_STACK_SIZE - 1] = 0;
  activeNoteStackSize--;
}

// Removes all values from the note stack, and sets its index "pointer" back to 0
void clearNoteStack() {
  for (byte i = 0; i < MIDI_NOTE_STACK_SIZE; i++) {
    activeNoteStack[i] = 0;
  }
  
  activeNoteStackSize = 0;
}

// --- Outputting gate, trigger, and control voltages

// As long as at least one note is pressed, the gate is high,
// meaning that something somewhere should be letting audio
// through.
void updateGate() {
  digitalWrite(PIN_GATE, notesOn > 0 ? HIGH : LOW);
}

// Whenever a new note is pressed, we send out a trigger pulse,
// which allows things like ADSR envelopes to retrigger
void sendTriggerPulse() {
  digitalWrite(PIN_TRIGGER, HIGH);
  delay(1);
  digitalWrite(PIN_TRIGGER, LOW);
}

// Update the control voltage at the frequency output.
// This voltage works on a 1V/octave basis, meaning that if
// we go to a note one octave higher than the last, the voltage
// will be incremented by 1V.
void updateFreqOutput() {
  // Remap the given note as we can't cover the full
  // MIDI note range.
  byte note = remapNote(activeNote);
  
  // Retrieve the value from the precalculated values,
  // so we don't have to do float arithmetic every time
  // this function is called.
  dacWrite(DAC_ADDR_FREQ, noteOutputValues[note]);
}

void updateVelocityOutput() {
  dacWrite(DAC_ADDR_MOD3, currentVelocity << 3);
}

// Update a control voltage output for a given controller.
// These map to the PIN_MODx pins.
void outputControlChange(byte controllerNumber, byte value) {
  byte outputAddress;

  switch (controllerNumber) {
    case MIDIC_MODULATION: outputAddress = DAC_ADDR_MOD1; break;
    case MIDIC_VOLUME: outputAddress = DAC_ADDR_MOD2; break;
    
    // Commented out because we're using DAC_ADDR_MOD3 for velocity
    //case MIDIC_BALANCE: outputAddress = DAC_ADDR_MOD3; break;
    
    // If the controller isn't listed above, we can do nothing
    default: return;
  }

  // The value here comes from a MIDI value and will be
  // in the range of 0-127(0b01111111), shifting it to the left
  // effectively multiplies it by 8, getting 0-1024 range
  // with the same precision.
  dacWrite(outputAddress, value << 3);
}

// Special case controller, pitch bend.
// Pitch bend messages have 14 bit precision, spread across the
// the 2 data bytes. We re-map it to our available 10 bit precision.
void outputPitchBend(byte data1, byte data2) {
  unsigned int pitchValue = (data2 << 7 | data1) & 0b11111111111111;
  dacWrite(DAC_ADDR_MOD4, pitchValue / 16);
}

// Write a value to a DAC channel.
// dacChannel should be one of the DAC_ADDR_* constants.
// value should be 10 bit; this will be enforced by bitmasking.
void dacWrite(byte dacChannel, int value) {
  digitalWrite(PIN_DAC_LATCH, LOW);

  // According to LTC1660 datasheet, we send out the address,
  // then the value, then 2 irrelevant bits, making up a
  // 16 bit message.
  word shiftedValue = (dacChannel & 0b1111) << 12;
  shiftedValue |= ((value << 2) & 0b111111111100);

  shiftOut(PIN_DAC_DATA, PIN_DAC_CLK, MSBFIRST, shiftedValue >> 8);
  shiftOut(PIN_DAC_DATA, PIN_DAC_CLK, MSBFIRST, shiftedValue & 0b11111111);

  digitalWrite(PIN_DAC_LATCH, HIGH);
}

// --- MIDI channel selection

// Interrupt handler for when the button is pressed.
// Contains logic for debouncing (eliminating the ever-present jitter when pressing a switch)
// After debouncing, increments the midiChannel by one.
void channelSelectButtonOnPressed() {
  // Debounce
  long currentTime = millis();
  if (currentTime - lastButtonPress < BUTTON_DEBOUNCE_MS) return;
  lastButtonPress = currentTime;

  setMIDIChannel(midiChannel + 1);
}

// Sets the midi channel to a new value,
// correcting any out-of-range values in the process.
// After changing to a new channel, resets all outputs,
void setMIDIChannel(byte newChannel) {
  if (newChannel > MIDI_CHANNELS || newChannel < 1) newChannel = 1;

  // Don't go resetting stuff if nothing actually changed
  if (midiChannel == newChannel) return;

  midiChannel = newChannel;
  updateDisplay();

  // Reset all the things!
  notesOn = 0;
  clearNoteStack();
  updateGate();
  dacWrite(DAC_ADDR_ALL, ANALOG_CENTER_VALUE);

  // Save the newly set MIDI channel to the EEPROM,
  // so it may be recovered next power-up
  EEPROM.write(MIDI_CHANNEL_EEPROM_ADDRESS, midiChannel);
}

// Write the currently selected midi channel number on the
// 7-segment display
void updateDisplay() {
  byte *digits = getDigitBytes(midiChannel);

  digitalWrite(PIN_DISP_LATCH, HIGH);

  // Shift out the bytes to the shift registers connected
  // to the displays.
  shiftOut(PIN_DISP_DATA, PIN_DISP_CLK, LSBFIRST, ~digits[1]);
  shiftOut(PIN_DISP_DATA, PIN_DISP_CLK, LSBFIRST, ~digits[0]);

  digitalWrite(PIN_DISP_LATCH, LOW);

  // Slightly hacky fix, but doing this makes the registers actually output
  // the newly shifted data. Maybe that's how it's meant to work.
  digitalWrite(PIN_DISP_LATCH, HIGH);
  digitalWrite(PIN_DISP_LATCH, LOW);
}

// Gets the two digit bytes representing a given number, for display
// on 7-segment display.
// Assumes number < 100. In this program we're only dealing with 1-16.
byte * getDigitBytes(byte number) {
  static byte digits[2];
  byte ones = number % 10;
  byte tens = (number - ones) / 10;

  digits[0] = DIGITS[tens];
  digits[1] = DIGITS[ones];
  return digits;
}
