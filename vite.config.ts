import { fileURLToPath } from "node:url";
import { defineConfig } from "vite";
import { svelte } from "@sveltejs/vite-plugin-svelte";

const repositoryRoot = fileURLToPath(new URL(".", import.meta.url));
const webRoot = fileURLToPath(new URL("./web", import.meta.url));

export default defineConfig({
  root: webRoot,
  plugins: [svelte()],
  server: {
    fs: {
      allow: [repositoryRoot],
    },
  },
  build: {
    outDir: fileURLToPath(new URL("./web/dist", import.meta.url)),
    emptyOutDir: true,
  },
});
