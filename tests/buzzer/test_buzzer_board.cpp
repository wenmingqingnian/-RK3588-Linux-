#include "../../device/buzzerdevice.h"

#include <cstdio>
#include <string>
#include <unistd.h>

int main()
{
	    vehicledevice::BuzzerConfig config;
	        config.enabled = true;
		    config.gpioChip = "/dev/gpiochip4";
		        config.gpioLine = 3;
			    config.activeLow = true;

			        vehicledevice::BuzzerDevice buzzer(config);
				    std::string error;

				        if (!buzzer.openDevice(&error)) {
						        std::fprintf(stderr, "open failed: %s\n", error.c_str());
							        return 1;
								    }

					    std::puts("silent for 1 second");
					        ::sleep(1);

						    if (!buzzer.setAlarm(true, &error)) {
							            std::fprintf(stderr, "alarm-on failed: %s\n", error.c_str());
								            return 2;
									        }

						        std::puts("beep for 200 ms");
							    ::usleep(200000);

							        if (!buzzer.setAlarm(false, &error)) {
									        std::fprintf(stderr, "alarm-off failed: %s\n", error.c_str());
										        return 3;
											    }

								    std::puts("silent for 1 second");
								        ::sleep(1);

									    std::puts("buzzer board test passed");
									        return 0;
}
