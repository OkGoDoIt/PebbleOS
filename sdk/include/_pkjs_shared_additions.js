/**
 * Copyright 2024 Google LLC
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

(function(p) {
  if (!p === undefined) {
    console.error('Pebble object not found!?');
    return;
  }

  // Aliases:
  p.on = p.addEventListener;
  p.off = p.removeEventListener;

  // For Android (WebView-based) pkjs, print stacktrace for uncaught errors:
  if (typeof window !== 'undefined' && window.addEventListener) {
    window.addEventListener('error', function(event) {
      if (event.error && event.error.stack) {
        console.error('' + event.error + '\n' + event.error.stack);
      }
    });
  }

  if (typeof _PebbleAudioContext !== 'undefined' && !p.audioContext) {
    var callbacks = {};
    var events = {};
    var nextId = 1;

    window._PebbleAudioContextCB = {
      _resultSuccess: function(id, payloadJson) {
        var cb = callbacks[id];
        delete callbacks[id];
        if (cb) {
          cb.resolve(JSON.parse(payloadJson));
        }
      },
      _event: function(subscriptionId, payloadJson) {
        if (events[subscriptionId]) {
          events[subscriptionId](JSON.parse(payloadJson));
        }
      }
    };

    function request(method, arg) {
      return new Promise(function(resolve, reject) {
        var id = '' + nextId++;
        callbacks[id] = { resolve: resolve, reject: reject };
        if (arg === undefined) {
          _PebbleAudioContext[method](id);
        } else {
          _PebbleAudioContext[method](id, JSON.stringify(arg));
        }
      });
    }

    p.audioContext = {
      getStatus: function() {
        return request('getStatus');
      },
      requestEnable: function() {
        return request('requestEnable');
      },
      requestPermission: function(permissions) {
        return request('requestPermission', permissions || []);
      },
      recentTranscript: function(options) {
        return request('recentTranscript', options || {});
      },
      transcriptHistory: function(options) {
        return request('transcriptHistory', options || {});
      },
      onTranscript: function(options, handler) {
        return request('subscribeTranscript', options || {}).then(function(result) {
          events[result.subscriptionId] = function(event) {
            handler(event.segment);
          };
          return function() {
            delete events[result.subscriptionId];
            _PebbleAudioContext.unsubscribe(result.subscriptionId);
          };
        });
      }
    };
  }

})(Pebble);
