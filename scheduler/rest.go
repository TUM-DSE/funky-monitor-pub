package main

import (
    "fmt"
    "io/ioutil"
    "net/http"
    "net"
    "log"
    "strconv"
)

const SockAddr = "/tmp/front.sock"

func uploadFile(w http.ResponseWriter, r *http.Request) {
    fmt.Println("File Upload Endpoint Hit")

    // Parse our multipart form, 10 << 20 specifies a maximum
    // upload of 10 MB files.
    r.ParseMultipartForm(1000 << 20)
    // FormFile returns the first file for the given key `myFile`
    // it also returns the FileHeader so we can get the Filename,
    // the Header and the size of the file
    file, handler, err := r.FormFile("myFile")
    if err != nil {
        fmt.Println("Error Retrieving the File")
        fmt.Println(err)
        return
    }
    defer file.Close()
    prioS := r.FormValue("Priority");
    prio, err := strconv.Atoi(prioS)
    if err != nil {
        // handle error
        fmt.Println(err)
	return;
    }
    fmt.Printf("Uploaded File: %+v\n", handler.Filename)
    fmt.Printf("File Size: %+v\n", handler.Size)
    fmt.Printf("MIME Header: %+v\n", handler.Header)
    fmt.Printf("Priority: %d\n", prio)

    // Create a temporary file within our temp-images directory that follows
    // a particular naming pattern
    tempFile, err := ioutil.TempFile("/tmp/", "upload-*.ukvm")
    if err != nil {
        fmt.Println(err)
    }
    defer tempFile.Close()

    // read all of the contents of our uploaded file into a
    // byte array
    fileBytes, err := ioutil.ReadAll(file)
    if err != nil {
        fmt.Println(err)
    }
    // write this byte array to our temporary file
    tempFile.Write(fileBytes)
    // return that we have successfully uploaded our file!
    fmt.Fprintf(w, "Successfully Uploaded File\n")

    c, err := net.Dial("unix", SockAddr)
    if err != nil {
	    log.Fatal("Dial error", err)
    }

    s := fmt.Sprintf("New: %s priority: %d", tempFile.Name(), prio)
    _, err = c.Write([]byte(s))
    if err != nil {
	    log.Fatal("Write error", err)
    }
    defer c.Close()
}

func setupRoutes() {
    http.HandleFunc("/upload", uploadFile)
    http.ListenAndServe(":8080", nil)
}

func main() {
    fmt.Println("Hello World")
    setupRoutes()
}
