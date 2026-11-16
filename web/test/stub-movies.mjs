// Stand-in for web/lib/movies.js (owned by another agent): same contract, fake MP4 bytes.
export const EARLY_MOVIES = ['P3CTOP2', 'P3CTOP', 'GAME_START', 'P3CT001_01', 'P3CT001_02'];
export const MOVIE_RECIPE_VERSION = 'stub-movie-recipe-v0';
export async function createMovieConverter({ onLog } = {}) {
  onLog?.('stub converter ready');
  return {
    async convert(stem, sfd, onProgress) {
      for (let i = 1; i <= 4; i++) { onProgress(i / 4); await new Promise((r) => setTimeout(r, 5)); }
      return new TextEncoder().encode(`FAKE-MP4 ${stem} from ${sfd.length} bytes of SFD`);
    },
    terminate() {},
  };
}
