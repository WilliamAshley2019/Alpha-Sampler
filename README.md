TRICK OR TREAT OCTOBER 31st 2026

ADDED PYTHON SCRIPTS I added some starter editor scripts
OPEN YOUR SCRIPT FOLDER IT IS LIKELY IN YOUR DOCUMENTS FOLDER documents/alphasampler/scripts   drop them into that folder then it will detect and you can use them. I'll add more scripts at some point, I have a list to work from 

Note you need to AFAIK have python installed I make scripts for python 3.

You can likely make scripts for whatever version of python you have installed of course but I am not cause I don't use those versions of python.
SEE SCRIPT_FORMAT.md
script_template.py

Specific scripts added so far.. more to come as available currently testing these I think 

bit_crush.py

channel_swap.py

dc_remove.py

FadeIn.py

FadeOut.py

Gain.py

haas.py

highpass.py

LoopCrossfade.py

lowpass.py

mid_side_encode.py

pan.py

PeakNormalize.py

PitchShift.py

polarize.py

resample.py

Reverse.py

ring_mod.py

RMSNormalize.py

SoftClip.py

stereo_to_mono.py THIS ONE DOESNT WORK NEED TO FINISH IT

stereo_width.py

TempoSync.py

TrimSilence.py

ZeroCrossingTrim.py




Version 1.0.5

-Added individual slice selection

-Added add remove slice markers

-Added drag and drop slice or sample into daw

-Samples now retain slice data both for load and export.

Issues: I still havn't addressed the record function need to figure out how to go about that specifically as routing audio into a plugin from FL Studio is something I had to use global memory for in the past not sure if there is another way as routing has output but I can't find input into generators may need to loop back trhough paired effect plugin to handle audio bus routing functions.

To do build partner plugin to act as loopback insert to handle audio handoff for inputs into the generator (defies fl studio state preventing feedback but its possible to bypass this issue using external memory.

NOTE:  To be honest I am a little impressed with how capable this sampler is now running a few % CPU ... it actually gave me that feeling wow, pretty darn impressive for what it is........ I will have to test for a while to get a sense for it. And wh


Version 1.0.4

Added slicing its in the MISC tab. 

Added slice exports so samples can not be exported 
~~TODO move slice markers manually.~~
~~TODO export WITH slice markers written the the wav file.~~ compatible with other formats such as REX.

render and RECORD Function etc.. HOWEVER USE A DIFFERENT INSTANCE OF ALPHA SAMPLER TO RECORD!!!!!!! it will overwrite your current sample loaded if your record in a used alpha sampler!!!! 
TO DO ADD UNDO RECORD OVERWRITE 
ADDED PYTHON  SCRIPTING THIS IS STILL EXPERIMENTAL!!!!!


Version 1.0.3

Added a few more visual elements

Added presets



Version 1.0.1 (or is it 1.0.2) hmmm
Ah it dawned on me anyone wanting to build might want to know it was made in JUCE 8.0.12 (I'm too lazy to update it seems to work fine for me currently)  built and tested in windows 11.  Using visual studio 2026 the source is a little picky probably won't build in v17 USE C++20   Add DSP with basics in the jucer and it should work, I might add the jucer later but nothing really crazy on this.
Folders are just as they are appear  SOURCE  and third_party exactly as packaged. don't put third_party in source. 

Added GUI and drag and drop.  The next big tasks are:

~~TODO add slice functions.... and~~
TODO see what I missed in terms of functionality that might be extractable from Chan Sampler or other GPL3 projects. One thing was addding the multicolour functions
on the sample from chan sampler.  I will likely need to let this fester and breed in my mind for a while to figure out what exactly I want to use with it.
The slicing is a 100% to do though.

~~TODO want to add sample export / render~~ 


~~TODO add sample render/export from the sampler.~~
~~TODO  the ever elusive integration with python scripting.. hmm need to see if edison scripts can some how be integrated.~~


This is very early but audio functions and basic GUI seem to be working

~~TODO add drag and drop feature~~   DONE

TODO add more visual representations in the scope section STILL CAN IMPROVE THE SCOPE
TODO double check the loop modes start knob seems like it may not be linked. 
I may want to rework some of the visual layout currently as GUI layout may not be ideal.

NOTE: This was based off of Lemon Studios sampler and involved a large port and reworking. 
https://github.com/ItsAlphaNeon/Lemon-Studio
I mention what inspired it so as to not confuse it with Chan Sampler - which I actually would like to take a look at
and see if there is anything I can borrow from Chan Sampler I havn't yet implemented into the plugin.
https://github.com/MaymiPR/ChanSampler    this is a really cool project that for whatever reason I can't help but laugh everytime I see it and think you did not clone it that much.


Its still sort of CPU heavy at 4% so I will likely need to see what I can do to reduce the CPU load such as 
some better form of sleeping non active elements.

Currently pretty early into testing and it seems most of the functions are currently working
some pretty cool sound effects and it is feeling pretty smooth.

Note SignalSmith Stretch code was used rather than writing custom stretch functions - it is under MIT License see the license there. https://github.com/Signalsmith-Audio/signalsmith-stretch

