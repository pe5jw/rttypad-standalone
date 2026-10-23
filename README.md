# This is afork from Rttypad bj DL1BZ, its a test to extend rttypad with tx.

Trying to port the rtty-enigene from deskhpsr to add to rttypad so its works on tci.
It seams to use portaudo so its als swtched to mniaudio.

Planning :) BoltTCIAddon, digimodes and rtty/cw with a winkey emulator to sent the code from a logger like n1mm+.
Just playing around the code wont be stabele so don't use it !!! :)
If you do feedback would be nice.

Please dont bother DL1BZ with problems in this code its not made by him !!!

# rttyPad for deskHPSDR by DL1BZ

rttyPad for deskHPSDR is an extension app for RTTY in combination with my deskHPSDR SDR app. rttyPad is simply the Open Source version and Qt6 porting of my rttyTCI, which is published as Freeware exclusive for macOS native only (but not Open Source). rttyPad can run with macOS, Linux and WINDOWS unlike rttyTCI, which work only with macOS as native Mac app.

<img src="Resources/screenshot.png" width="1024px" />

## The concept behind - what it's make for and what it isn't for

rttyPad requires deskHPSDR as backend, because it use the since deskHPSDR version 2.8.x build-in RTTY TX engine. RX audio for decoding RTTY need TCI audio streaming. Analogue audio support is not available. In contrast to rttyTCI, which has an own RTTY TX encoder build-in and can work out-of-the-box with every app which provide TCI audio streaming in RX and TX directions, rttyPad need deskHPSDR as frontend. It's made as an extension app especially and exclusive for deskHPSDR with the following basic options
- send/encode (deskHPSDR required by using it's build-in RTTY TX engine) and receive/decode RTTY using TCI only, no analogue audio support
- macro support for design your own text templates
- own logging with ADIF log export or UDP live log send in Flexradio format
- Contest module with own logging, special contest macros and Cabrillo export

rttyPad is not (and never will be) made as an universal RTTY app like MMTTY or similar apps (Fldigi, iDigi), which support RTTY as mode.

**Note: deskHPSDR is a dedicated SDR transceiver frontend application using OpenHPSDR protocols 1 or 2 for everyday use in amateur radio.**

## Latest Changes

We start now in state "early development" with version 0.1.0

In this state my clear rules are the following:
- don't accept user issue reports
- don't accept any kind of user requests, PR or whatever
- I only answer questions about operation, but none about building the app

The final concept for this app is simply not ready now. All around this is currently "in progress", not more, not less. However, I am giving interested users the opportunity to participate in the development of this app. It's not a "preview", the app works from my point of view, I made successfully some contests with rttyTCI, which was and is the main code base of rttyPad.

## Credits

The complete rttyPad development was made exclusive by myself, none of foreign code will be used, all code is fully under my own copyright.

Special thanks to:
- my lovely wife any my family for her great patience and understanding
- Expert Electronics for their TCI protocol development
- OpenAI/ChatGPT for brainstorming, code optimizations and save me a lot of time in development
- all of the active users and contributors for support my deskHPSDR

## Exclusion of any Guarantee and any Warrenty and limited Support

This program is free software: you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, either version 3 of the License, or (at your option) any later version.<br>

This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for more details.<br>

All what you do with this code is at your very own risk. The code is published "as it is" without right of any kind of support or similiar services.<br>

**There are no rights or obligations to get any kind of user support for rttyPad from me, I publish only the app source code "as it is".**
