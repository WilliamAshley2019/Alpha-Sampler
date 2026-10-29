Version 1.0.1 (or is it 1.0.2) hmmm
Ah it dawned on me anyone wanting to build might want to know it was made in JUCE 8.0.12 (I'm too lazy to update it seems to work fine for me currently)  built and tested in windows 11.  Using visual studio 2026 the source is a little picky probably won't build in v17 USE C++20   Add DSP with basics in the jucer and it should work, I might add the jucer later but nothing really crazy on this.
Folders are just as they are appear  SOURCE  and third_party exactly as packaged. don't put third_party in source. 

Added GUI and drag and drop.  The next big tasks are:

TODO add slice functions.... and
TODO see what I missed in terms of functionality that might be extractable from Chan Sampler or other GPL3 projects. One thing was addding the multicolour functions
on the sample from chan sampler.  I will likely need to let this fester and breed in my mind for a while to figure out what exactly I want to use with it.
The slicing is a 100% to do though.

TODO want to add sample export / render 


TODO add sample render/export from the sampler.

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

