This is very early but audio functions and basic GUI seem to be working

TODO add drag and drop feature
TODO add more visual representations in the scope section

I may want to rework some of the visual layout currently as GUI layout may not be ideal.

NOTE: This was based off of Lemon Studios sampler and involved a large port and reworking. 
https://github.com/ItsAlphaNeon/Lemon-Studio
I mention what inspired it so as to not confuse it with Chan Sampler - which I actually would like to take a look at
and see if there is anything I can borrow from Chan Sampler I havn't yet implemented into the plugin.
https://github.com/MaymiPR/ChanSampler    this is a really cool project that for whatever reason I can help but laugh and think you did not clone it that much.


Its still sort of CPU heavy at 4% so I will likely need to see what I can do to reduce the CPU load such as 
some better form of sleeping non active elements.

Currently pretty early into testing and it seems most of the functions are currently working
some pretty cool sound effects and it is feeling pretty smooth.

Note SignalSmith Stretch code was used rather than writing custom stretch functions - it is under MIT License see the license there. https://github.com/Signalsmith-Audio/signalsmith-stretch
