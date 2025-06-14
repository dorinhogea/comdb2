/*
   Copyright 2024 Bloomberg Finance L.P.

   Licensed under the Apache License, Version 2.0 (the "License");
   you may not use this file except in compliance with the License.
   You may obtain a copy of the License at

       http://www.apache.org/licenses/LICENSE-2.0

   Unless required by applicable law or agreed to in writing, software
   distributed under the License is distributed on an "AS IS" BASIS,
   WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
   See the License for the specific language governing permissions and
   limitations under the License.
 */

#ifndef __STR_UTIL_H__
#define __STR_UTIL_H__

#define QUOTE_(x) #x
#define QUOTE(x) QUOTE_(x)

int str_has_ending(const char * const str, const char * const ending);

// Checks that a string is alphanumeric. Allowed non-alphanumeric characters can be passed in `exceptions`.
// Pass emptystring to `exceptions` to pass no exceptions.
int str_is_alphanumeric(const char * const name, const char * const exceptions);

#endif

