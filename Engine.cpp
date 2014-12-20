#include "Arduino.h"
#include "Engine.h"

Engine::Engine()
{
	rnd = new Rand();
	rnd->seed(millis());

	// Memory, and display objects (I2C devices)
	memory = new Memory(EEPROM_I2C_ADDRESS);

	snapshot = new Snapshot(memory);

	dual_display_driver = new DualDisplayDriver(DISPLAY_I2C_ADDRESS, snapshot);

	sequencer = new Sequencer();
	transposer = new Transposer(snapshot);
	transposer2 = new Transposer(snapshot);

	transposer2->clock_division = 2;

	// Rotary encoder objects
	step_encoder  = new RotaryEncoder(A0, A1, A2);  // top encoder
	value_encoder = new RotaryEncoder(8, 9, 10);   // bottom encoder

	// Trigger input objects
	clock_button = new TriggerInput(PIN_CLOCK_BUTTON);
	clock_input  = new TriggerInput(PIN_CLOCK_INPUT);
	reset_button = new TriggerInput(PIN_RESET_BUTTON);
	reset_input  = new TriggerInput(PIN_RESET_INPUT);

	// Change the reset and clock input's debounce time to a 
	// slim 1 millilsecond. (The default is 50 milliseconds,
	// which is fine for pushbuttons, but too slow for other uses.)
	reset_input->setDebounce(1);
	clock_input->setDebounce(1);

	// Mode switch
	mode_switch = new SwitchInput(PIN_SWITCH_A, PIN_SWITCH_B);

	// Output object
	output = new Output(DAC_I2C_ADDRESS, snapshot, transposer, transposer2);

}

void Engine::init()
{
	// I2C setup
	Wire.begin();

	snapshot->init();
	step_encoder->init();
	value_encoder->init();
	sequencer->init();

	// Initialize display driver and set displays
	dual_display_driver->init();

	// Initialize output, which loads the scale setting from non-volitile ram
	// output->init();

	// Test to see if the reset button is being held or
	// if the initialized flag is not set to "22" in memory.
	// If either is true, initialize the unit by setting all values to 0,
	// the length to 8, and the clock division to 1.
	
	uint8_t self_initialization_flag = memory->read(MEM_ADDR_SELF_INITIALIZATION);
	
	if(reset_button->read() || self_initialization_flag != 22) 
	{
		this->factoryReset();
		memory->write(MEM_ADDR_SELF_INITIALIZATION, 22);
	}

	// Initialize rotary encoder sensitivity values
	z_sequence_length = snapshot->sequence_length << 1;
	z_clock_division = snapshot->clock_division << 1;
	z_scale = snapshot->scale << 1;
	z_intensity = snapshot->display_intensity << 1;
	z_hold = snapshot->hold << 1;
	z_song = snapshot->song << 1;
	z_song2 = snapshot->song2 << 1;

	// Show a welcome message very briefly
	dual_display_driver->writeByteCode(4, 0b00101111); // H
	dual_display_driver->writeByteCode(5, 0b01001111); // E
	dual_display_driver->writeByteCode(6, 0b00001110); // L
	dual_display_driver->writeByteCode(7, 0b01111110); // O

	delay(500);

	value = snapshot->sequence[step];
	output->write(value);
}

void Engine::loop()
{
	mode_switch->poll();

	switch(mode_switch->position)
	{
		case 1:
			mode = SETTINGS_MODE;
			break;
		case 2:
			if(mode != SEQUENCE_EDIT_MODE) 
			{
				edit_step = step;
				edit_value = snapshot->sequence[step];
				mode = SEQUENCE_EDIT_MODE;
			}
			break;
		case 3:
			if(mode != SEQUENCE_PLAYBACK_MODE) 
			{
				value = snapshot->sequence[step];
				mode = SEQUENCE_PLAYBACK_MODE;
			}			
			break;
	}

	switch(mode)
	{
		case SEQUENCE_PLAYBACK_MODE:
			this->sequencePlaybackMode();
			break;
		case SEQUENCE_EDIT_MODE:
			this->sequenceEditMode();
			break;
		case SETTINGS_MODE:
			this->settingsMode();
			break;
	}

	// Constantly playback when clocked, independent of the mode
	this->playback();
}

//
// Engine::playback()
//
// This method is called every loop cycle.  It's in charge of  keeping the 
// sequence playing.  Not to be confused with playback_mode, which is the mode 
// where users can watch the sequence playback.
//

void Engine::playback()
{
	clock_input->poll();
	reset_input->poll();

	// Step the sequencer using either the clock button or clock input
	if(clock_input->triggered)
	{
		// Step the internal clock variable
		clock_counter = clock_counter + 1;

		// Once the clock division has been reached,
		// increment the step variable
		if(clock_counter >= snapshot->clock_division)
		{
			if(snapshot->slip == 0 || (rnd->random(100) > snapshot->slip)) // if no slip
			{
				// Step sequencer
				step = step + 1;

				if(step >= snapshot->sequence_length)
				{
					step = 0;

					// Step transposers
					transposer->clock();
					transposer2->clock();
				}

				// Apply drift
				if(snapshot->drift_percentage && (rnd->random(100) < snapshot->drift_percentage))
				{
					sequencer->drift[step] = sequencer->drift[step] + ((int) rnd->random(snapshot->drift_amount << 1)) - snapshot->drift_amount;
				}
			}

			// "hold" is the sample and hold 8-bit pattern
			// A hold of 0 means that the sample & hold feature is turned off
			if((!snapshot->hold) || bitRead(snapshot->hold, step % 8))
			{
				// Get the value from the sequencer
				value = snapshot->sequence[step];
				drift = sequencer->drift[step];

				int32_t total = value + drift;
				total = constrain(total, 0, 4095);

				output->write(total);
			}

			// Reset clock_counter
			clock_counter = 0;
		}
	}	

	if(reset_input->triggered)
	{
		step = 0;
		sequencer->resetDrift();
		transposer->reset();
		transposer2->reset();
		value = snapshot->sequence[step];
		output->write(value);
	}	
}

//
// Engine::sequencePlaybackMode()
//
// This mode lets the user to watch the sequence playback on the bubble displays.
// The user can manually reset the sequence in this mode using the reset button.
// The user can manually step the sequence in this mode using the step button.
// Neither of the rotary encoders do anything in this mode.
//

void Engine::sequencePlaybackMode()
{
	// Poll buttons
	clock_button->poll();
	reset_button->poll();

	// Step the sequencer using the clock button
	if(clock_button->triggered)
	{
		step = (step + 1) % snapshot->sequence_length;

		transposer->step();
		transposer2->step();
		
		clock_counter = 0;

		// Get the value from the sequencer
		value = snapshot->sequence[step];
		drift = sequencer->drift[step];

		int32_t total = value + drift;
		total = constrain(total, 0, 4095);

		output->write(total);		
	}

	if(reset_button->triggered)
	{
		step = 0;
		// sequencer->setStep(step);
		clock_counter = 0;
		sequencer->resetDrift();
		transposer->reset();
		transposer2->reset();
		value = snapshot->sequence[step];
		output->write(value);
	}

	dual_display_driver->write(TOP_DISPLAY, step + 1);
	dual_display_driver->write(BOTTOM_DISPLAY, value);
}

//
// Engine::sequencePlaybackMode()
//
// This mode lets the user to edit the sequencer values.
//

void Engine::sequenceEditMode()
{
	// Poll buttons
	clock_button->poll();
	reset_button->poll();

	int16_t step_acceleration = 1;
	int16_t value_acceleration = 1;

	if(step_encoder->readButton() && (snapshot->sequence_length > 16)) step_acceleration = 10;
	if(value_encoder->readButton()) value_acceleration = 100;

	z_edit_step = (z_edit_step + (step_encoder->read() * step_acceleration));

	// Step the sequencer using the clock button
	if(clock_button->triggered) z_edit_step += 2;
	if(reset_button->triggered) z_edit_step -= 2;

	z_edit_step = constrain(z_edit_step, 0, (snapshot->sequence_length << 1) - 1);

	uint16_t edit_step = z_edit_step >> 1;

	// Get the value from the sequencer
	edit_value = snapshot->sequence[edit_step]; // sequencer->getValue(edit_step);

	// Read value encoder and adjust value (bottom knob/display)
	edit_value = (edit_value + (value_encoder->read() * value_acceleration));
	edit_value = constrain(edit_value, 0, 4095);

	// Update the sequencer with the new value
	// sequencer->setValue(edit_step, edit_value);
	snapshot->setValue(edit_step, edit_value);

	dual_display_driver->write(TOP_DISPLAY, edit_step + 1);
	dual_display_driver->write(BOTTOM_DISPLAY, edit_value);	
}

//
// Engine::settingsMode()
//
// This mode lets the user to edit the module's settings.
// Settings are organized into pages, which are selected via the
// top rotary encoder or the step button.
//

void Engine::settingsMode()
{
	// Sequence length
	if(settings_page == 0)
	{
		int sequence_length_acceleration = 1;

		// Rotary encoder buttons are used for input accelleration
		if(value_encoder->readButton()) sequence_length_acceleration = 20;

		// Set sequence length
		//
		// The variable 'z_sequence_length. is 2X the sequence length, and controls
		// the sensitivity of the rotary encoder, which can be too sensitive at times.

		// z_sequence_length = sequencer->getLength();
		z_sequence_length = z_sequence_length + (value_encoder->read() * sequence_length_acceleration);
		z_sequence_length = constrain(z_sequence_length, 2, MAX_SEQUENCE_LENGTH << 1);

		uint16_t sequence_length = z_sequence_length >> 1;

		// sequencer->setLength(sequence_length);

		snapshot->setSequenceLength(sequence_length);

		// Update displays
		dual_display_driver->writeByteCode(0, 0b00001110); // L
		dual_display_driver->writeByteCode(1, 0b01001111); // E
		dual_display_driver->writeByteCode(2, 0b00010101); // n
		dual_display_driver->writeByteCode(3, 0b01111011); // g

		dual_display_driver->write(BOTTOM_DISPLAY, sequence_length);
	}

	// Clock division settings
	if(settings_page == 1)
	{
		int clock_division_acceleration = 1;

		// Rotary encoder buttons are used for input accelleration
		if(value_encoder->readButton()) clock_division_acceleration = 20;

		// Set clock division
		// clock_division = sequencer->getClockDivision();
		z_clock_division = z_clock_division + (value_encoder->read() * clock_division_acceleration);
		z_clock_division = constrain(z_clock_division, 2, 512 << 1);
		
		uint16_t clock_division = z_clock_division >> 1;

		snapshot->setClockDivision(clock_division);

		// Update displays
		dual_display_driver->writeByteCode(0, 0b00001101); // c
		dual_display_driver->writeByteCode(1, 0b00111101); // d
		dual_display_driver->writeByteCode(2, 0b00010000); // i
		dual_display_driver->writeByteCode(3, 0b00011100); // v

		dual_display_driver->write(BOTTOM_DISPLAY, clock_division);
	}

	// Scale settings
	if(settings_page == 2)
	{
		// Set scale
		z_scale = z_scale + (value_encoder->read());
		z_scale = constrain(z_scale, 0, NUMBER_OF_SCALES << 1);
		
		uint16_t scale = z_scale >> 1;

		snapshot->setScale(scale);

		dual_display_driver->writeByteCode(0, 0b01011011); // S
		dual_display_driver->writeByteCode(1, 0b01001110); // C
		dual_display_driver->writeByteCode(2, 0b01110111); // A
		dual_display_driver->writeByteCode(3, 0b00001110); // L

		dual_display_driver->write(BOTTOM_DISPLAY, scale);
	}

	// Randomize
	if(settings_page == 3)
	{
		dual_display_driver->writeByteCode(0, 0b00000101); // r
		dual_display_driver->writeByteCode(1, 0b01110111); // A
		dual_display_driver->writeByteCode(2, 0b00010101); // n
		dual_display_driver->writeByteCode(3, 0b00111101); // d

		dual_display_driver->writeByteCode(4, 0b00000001); // -
		dual_display_driver->writeByteCode(5, 0b00000001); // -
		dual_display_driver->writeByteCode(6, 0b00000001); // -
		dual_display_driver->writeByteCode(7, 0b00000001); // -

		if(value_encoder->released())
		{
			// Load sequence from non-volitile ram
			for(uint8_t i=0; i<MAX_SEQUENCE_LENGTH; i++)
			{
				uint16_t value = random(4096);
				snapshot->setValue(i, value);
				dual_display_driver->write(BOTTOM_DISPLAY, value);
			}
		}
	}	

	// Clear
	if(settings_page == 4)
	{
		dual_display_driver->writeByteCode(0, 0b01001110); // C
		dual_display_driver->writeByteCode(1, 0b00001110); // L
		dual_display_driver->writeByteCode(2, 0b01001111); // E
		dual_display_driver->writeByteCode(3, 0b00000101); // r

		dual_display_driver->writeByteCode(4, 0b00000001); // -
		dual_display_driver->writeByteCode(5, 0b00000001); // -
		dual_display_driver->writeByteCode(6, 0b00000001); // -
		dual_display_driver->writeByteCode(7, 0b00000001); // -

		if(value_encoder->released())
		{
			// Load sequence from non-volitile ram
			for(int i=0; i<MAX_SEQUENCE_LENGTH; i++)
			{
				snapshot->setValue(i, 0);
				dual_display_driver->write(BOTTOM_DISPLAY, i);
			}
		}
	}		

	// Slip
	if(settings_page == 5)
	{
		dual_display_driver->writeByteCode(0, 0b01011011); // S
		dual_display_driver->writeByteCode(1, 0b00001110); // L
		dual_display_driver->writeByteCode(2, 0b00010000); // i
		dual_display_driver->writeByteCode(3, 0b01100111); // P

		// Set slip
		int8_t slip = snapshot->slip + (value_encoder->read());
		slip = constrain(slip, 0, 99);
		snapshot->setSlip(slip);

		dual_display_driver->write(BOTTOM_DISPLAY, slip);
	}


	// Drift Percentage
	if(settings_page == 6)
	{
		int16_t drift_percentage_acceleration = 1;
		if(value_encoder->readButton()) drift_percentage_acceleration = 10;

		dual_display_driver->writeByteCode(0, 0b00111101); // d
		dual_display_driver->writeByteCode(1, 0b00000101); // r
		dual_display_driver->writeByteCode(2, 0b00000001); // -
		dual_display_driver->writeByteCode(3, 0b01100111); // P

		int8_t drift_percentage = snapshot->drift_percentage + (value_encoder->read() * drift_percentage_acceleration);
		drift_percentage = constrain(drift_percentage, 0, 100);
		snapshot->setDriftPercentage(drift_percentage);

		dual_display_driver->write(BOTTOM_DISPLAY, drift_percentage);
	}	

	// Drift Amount
	if(settings_page == 7)
	{
		int16_t drift_acceleration = 1;
		if(value_encoder->readButton()) drift_acceleration = 100;

		dual_display_driver->writeByteCode(0, 0b00111101); // d
		dual_display_driver->writeByteCode(1, 0b00000101); // r
		dual_display_driver->writeByteCode(2, 0b00001000); // _
		dual_display_driver->writeByteCode(3, 0b01110111); // A

		int16_t drift_amount = snapshot->drift_amount;
		drift_amount = drift_amount + (value_encoder->read() * drift_acceleration);
		drift_amount = constrain(drift_amount, 0, 300);

		snapshot->setDriftAmount(drift_amount);

		dual_display_driver->write(BOTTOM_DISPLAY, drift_amount);
	}

	// Hold pattern
	if(settings_page == 8)
	{
		int16_t hold_acceleration = 1;
		if(value_encoder->readButton()) hold_acceleration = 20;

		dual_display_driver->writeByteCode(0, 0b00010111); // h
		dual_display_driver->writeByteCode(1, 0b00011101); // o
		dual_display_driver->writeByteCode(2, 0b00110000); // l
		dual_display_driver->writeByteCode(3, 0b00111101); // d

		z_hold = z_hold + (value_encoder->read() * hold_acceleration);
		z_hold = constrain(z_hold, 0, 255 << 1);
		
		uint16_t hold = z_hold >> 1;

		snapshot->setHold(hold);

		dual_display_driver->write(BOTTOM_DISPLAY, hold);
	}


	// Song pattern
	if(settings_page == 9)
	{
		int encoder_value = value_encoder->read();

		if(encoder_value != 0)
		{
			z_song = z_song + encoder_value;
			z_song = constrain(z_song, 0, (NUMBER_OF_SONGS-1) << 1); // 0 == off
			
			snapshot->setSong(z_song >> 1);
		}

		dual_display_driver->writeByteCode(0, 0b01011011); // S
		dual_display_driver->writeByteCode(1, 0b00011101); // o
		dual_display_driver->writeByteCode(2, 0b00010101); // n
		dual_display_driver->writeDigit(3, 1);             // 1

		dual_display_driver->write(BOTTOM_DISPLAY, z_song >> 1);
	}

	// Song2 pattern
	if(settings_page == 10)
	{
		int encoder_value = value_encoder->read();

		if(encoder_value != 0)
		{
			z_song2 = z_song2 + encoder_value;
			z_song2 = constrain(z_song2, 0, (NUMBER_OF_SONGS-1) << 1); // 0 == off
			
			snapshot->setSong2(z_song2 >> 1);
		}

		dual_display_driver->writeByteCode(0, 0b01011011); // S
		dual_display_driver->writeByteCode(1, 0b00011101); // o
		dual_display_driver->writeByteCode(2, 0b00010101); // n
		dual_display_driver->writeDigit(3, 2);             // 2

		dual_display_driver->write(BOTTOM_DISPLAY, z_song2 >> 1);
	}		

	// LED Intensity
	if(settings_page == 11)
	{
		int encoder_value = value_encoder->read();
		// int8_t intensity = dual_display_driver->getIntensity();

		if(encoder_value != 0)
		{
			z_intensity = z_intensity + encoder_value;
			z_intensity = constrain(z_intensity, 0, 15 << 1);
			dual_display_driver->setIntensity(z_intensity >> 1);
			// snapshot->setDisplayIntensity(z_intensity >> 1);
		}

		dual_display_driver->writeByteCode(0, 0b00001110); // L
		dual_display_driver->writeByteCode(1, 0b01001111); // E
		dual_display_driver->writeByteCode(2, 0b00111101); // d
		dual_display_driver->writeByteCode(3, 0b01011011); // S

		dual_display_driver->write(BOTTOM_DISPLAY, z_intensity >> 1);
	}	

	// Select settings page using the clock button
	// Go back a page using the reset button
	clock_button->poll();
	reset_button->poll();

	if(clock_button->triggered) z_settings_page += 4;
	if(reset_button->triggered) z_settings_page -= 4;

	// You can also use the rotary top encoder to select the page
	z_settings_page = z_settings_page + step_encoder->read();
	z_settings_page = constrain(z_settings_page, 0, (NUMBER_OF_SETTINGS_PAGES << 2) - 1);

	// z_settings_page is used to slow down the reaction to the rotary encoder, which
	// normally could be a bit sensitive.
	settings_page = z_settings_page >> 2;

}

void Engine::factoryReset()
{

	snapshot->setClockDivision(1);
	snapshot->setSequenceLength(8);
	snapshot->setDriftPercentage(0);
	snapshot->setDriftAmount(0);
	snapshot->setSlip(0);
	snapshot->setScale(0);
	snapshot->setHold(0);
	snapshot->setSong(0);
	snapshot->setSong2(0);
	snapshot->setDisplayIntensity(15);

	for(int i=0; i<MAX_SEQUENCE_LENGTH; i++)
	{  
		snapshot->setValue(i, 0);
	}

	dual_display_driver->setIntensity(15);

	// Display rS__ for 2 seconds
	dual_display_driver->writeByteCode(4, 0b01000110); // r
	dual_display_driver->writeByteCode(5, 0b01011011); // s
	dual_display_driver->writeByteCode(6, 0b00001000); // _
	dual_display_driver->writeByteCode(7, 0b00001000); // _

	delay(2000);
}
